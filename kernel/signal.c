/*
 * signal.c — sending and delivering signals (see signal.h)
 *
 * Delivery, step by step, for a program with a handler for SIGINT:
 *   1. Ctrl-C: the terminal calls signal_send(); the bit for SIGINT is set
 *      in the process's "pending" mask, and if it was asleep it's woken
 *      (its syscall returns -EINTR).
 *   2. On its way back to user mode, signal_check() sees the bit. It saves
 *      the program's registers, mask and FPU state in a frame on the
 *      program's own stack — laid out exactly as Linux does, because the C
 *      library's code depends on it — and changes the return address to
 *      the handler. The handler's "return address" is the C library's
 *      restorer: a few instructions that call sigreturn.
 *   3. The handler runs as a normal function call.
 *   4. It returns into the restorer -> sigreturn -> the kernel reads the
 *      frame back and the program continues where it was interrupted.
 */
#include <errno.h>
#include <fpu.h>
#include <gdt.h>
#include <interrupts.h>
#include <kprintf.h>
#include <proc.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <thread.h>
#include <uaccess.h>

/* --- Linux's i386 signal frame -------------------------------------------- */

struct sigcontext {                 /* 88 bytes */
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip, cs, eflags, esp_at_signal, ss;
    uint32_t fpstate;               /* user address of the FPU state */
    uint32_t oldmask, cr2;
};

struct siginfo {                    /* 128 bytes; we fill signo and code */
    int32_t si_signo, si_errno, si_code;
    int32_t pad[29];
};

struct ucontext {
    uint32_t uc_flags, uc_link;
    uint32_t ss_sp, ss_flags, ss_size;
    struct sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};

struct sigframe {                   /* without SA_SIGINFO: handler(sig) */
    uint32_t pretcode;
    int32_t sig;
    struct sigcontext sc;
    uint32_t extramask;             /* the high half of the blocked mask */
};

struct rt_sigframe {                /* with SA_SIGINFO: handler(sig, info, uc) */
    uint32_t pretcode;
    int32_t sig;
    uint32_t pinfo, puc;
    struct siginfo info;
    struct ucontext uc;
};

/* EFLAGS bits a program may change through sigreturn (the rest — IOPL,
   VM, interrupt flag... — must stay as the kernel says). */
#define FLAGS_USER 0x00040DD5

/* Signals whose default action is to do nothing. */
static int default_ignored(int sig)
{
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH ||
           sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU || sig == SIGSTOP;
    /* (Stop signals are ignored until job control can stop processes.) */
}

/* --- sending ---------------------------------------------------------------- */

int signal_send(struct process *p, int sig)
{
    if (sig < 0 || sig > NSIG)
        return -EINVAL;
    if (sig == 0)
        return 0;                       /* "does it exist?" */
    uint32_t h = p->sig_action[sig - 1].handler;
    if (sig != SIGKILL && (h == SIG_IGN || (h == SIG_DFL && default_ignored(sig))))
        return 0;                       /* would be ignored: drop it now */

    uint32_t flags = irq_save();
    p->sig_pending |= SIGBIT(sig);
    if (sig == SIGKILL || !(p->sig_blocked & SIGBIT(sig)))
        if (p->thread && p->state == PROC_ALIVE)
            thread_wake(p->thread);     /* its sleep ends with -EINTR */
    irq_restore(flags);
    return 0;
}

int signal_pending(void)
{
    struct process *p = proc_current();
    return p && (p->sig_pending & ~p->sig_blocked);
}

/* --- delivering --------------------------------------------------------------- */

static void save_context(struct sigcontext *sc, const struct regs *r, uint64_t mask,
                         uint32_t fpstate)
{
    sc->gs = r->gs;   sc->fs = r->fs;   sc->es = r->es;   sc->ds = r->ds;
    sc->edi = r->edi; sc->esi = r->esi; sc->ebp = r->ebp; sc->esp = r->user_esp;
    sc->ebx = r->ebx; sc->edx = r->edx; sc->ecx = r->ecx; sc->eax = r->eax;
    sc->trapno = r->vector;
    sc->err = r->err_code;
    sc->eip = r->eip;
    sc->cs = r->cs;
    sc->eflags = r->eflags;
    sc->esp_at_signal = r->user_esp;
    sc->ss = r->user_ss;
    sc->fpstate = fpstate;
    sc->oldmask = (uint32_t)mask;
    sc->cr2 = 0;
}

/* Build the frame and point the program at its handler. 0, or -1 if the
   program's stack is unusable (then it dies with SIGSEGV). */
static int setup_frame(struct process *p, int sig, struct k_sigaction *a, struct regs *r)
{
    struct thread *t = thread_current();
    uint64_t mask = p->sig_restore ? p->sig_saved : p->sig_blocked;
    p->sig_restore = 0;

    /* The FPU registers: save them below the frame (16-byte aligned for
       fxsave), and give the handler a clean FPU. */
    uint32_t sp = (r->user_esp - FPU_STATE_SIZE) & ~0xFu;
    uint32_t fpstate = 0;
    if (t->uses_fpu) {
        fpu_save(t->fpu_state);
        if (copy_to_user((void *)sp, t->fpu_state, FPU_STATE_SIZE) < 0)
            return -1;
        fpstate = sp;
    }

    if (a->flags & SA_SIGINFO) {
        struct rt_sigframe f;
        memset(&f, 0, sizeof f);
        /* At the handler's first instruction, (esp + 4) must be 16-byte
           aligned, as after a normal `call`. */
        sp = ((sp - sizeof f + 4) & ~0xFu) - 4;
        f.pretcode = a->restorer;
        f.sig = sig;
        f.pinfo = sp + offsetof(struct rt_sigframe, info);
        f.puc = sp + offsetof(struct rt_sigframe, uc);
        f.info.si_signo = sig;
        f.info.si_code = 0;             /* SI_USER */
        save_context(&f.uc.uc_mcontext, r, mask, fpstate);
        f.uc.uc_sigmask = mask;
        if (copy_to_user((void *)sp, &f, sizeof f) < 0)
            return -1;
        r->edx = f.pinfo;               /* (also in registers, as Linux does) */
        r->ecx = f.puc;
    } else {
        struct sigframe f;
        memset(&f, 0, sizeof f);
        sp = ((sp - sizeof f + 4) & ~0xFu) - 4;
        f.pretcode = a->restorer;
        f.sig = sig;
        save_context(&f.sc, r, mask, fpstate);
        f.extramask = (uint32_t)(mask >> 32);
        if (copy_to_user((void *)sp, &f, sizeof f) < 0)
            return -1;
        r->edx = r->ecx = 0;
    }

    r->user_esp = sp;
    r->eip = a->handler;
    r->eax = sig;
    r->eflags &= ~(0x100 | 0x400);      /* no single-step, direction up */
    if (t->uses_fpu) {
        fpu_clean_state(t->fpu_state);
        fpu_restore(t->fpu_state);
    }

    /* While the handler runs, its mask (and the signal itself) are blocked. */
    p->sig_blocked |= a->mask;
    if (!(a->flags & SA_NODEFER))
        p->sig_blocked |= SIGBIT(sig);
    p->sig_blocked &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    if (a->flags & SA_RESETHAND)
        a->handler = SIG_DFL;
    return 0;
}

void signal_check(struct regs *r)
{
    struct process *p = proc_current();
    if (!p || !(r->cs & 3))
        return;
    for (;;) {
        uint64_t ready = p->sig_pending & ~p->sig_blocked;
        if (p->sig_pending & SIGBIT(SIGKILL))
            ready |= SIGBIT(SIGKILL);   /* can't be blocked */
        if (!ready) {
            if (p->sig_restore) {       /* sigsuspend ended without a handler */
                p->sig_blocked = p->sig_saved;
                p->sig_restore = 0;
            }
            return;
        }
        int sig = __builtin_ctzll(ready) + 1;   /* lowest number first */
        p->sig_pending &= ~SIGBIT(sig);

        struct k_sigaction *a = &p->sig_action[sig - 1];
        if (sig == SIGKILL || a->handler == SIG_DFL) {
            if (sig != SIGKILL && default_ignored(sig))
                continue;
            proc_exit(W_SIGNALED(sig));         /* the default: the end */
        }
        if (a->handler == SIG_IGN)
            continue;
        if (setup_frame(p, sig, a, r) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));     /* no usable stack */
        return;                                 /* one handler at a time */
    }
}

/* --- fork and exec ------------------------------------------------------------- */

void signal_fork(struct process *child, struct process *parent)
{
    memcpy(child->sig_action, parent->sig_action, sizeof child->sig_action);
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;             /* pending signals aren't inherited */
}

void signal_exec(struct process *p)
{
    /* The handlers were in the old program: gone. Ignored stays ignored. */
    for (int i = 0; i < NSIG; i++) {
        if (p->sig_action[i].handler != SIG_IGN)
            p->sig_action[i].handler = SIG_DFL;
        p->sig_action[i].flags = 0;
        p->sig_action[i].mask = 0;
    }
}

/* --- system calls --------------------------------------------------------------- */

int32_t sys_rt_sigaction_impl(int sig, uint32_t act, uint32_t oact, uint32_t size)
{
    struct process *p = proc_current();
    if (sig < 1 || sig > NSIG || size != 8)
        return -EINVAL;
    struct k_sigaction *a = &p->sig_action[sig - 1];
    if (oact && copy_to_user((void *)oact, a, sizeof *a) < 0)
        return -EFAULT;
    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        struct k_sigaction n;
        if (copy_from_user(&n, (void *)act, sizeof n) < 0)
            return -EFAULT;
        n.mask &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
        *a = n;
        if (n.handler == SIG_IGN || (n.handler == SIG_DFL && default_ignored(sig)))
            p->sig_pending &= ~SIGBIT(sig);     /* pending ones are dropped */
    }
    return 0;
}

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

int32_t sys_rt_sigprocmask_impl(int how, uint32_t set, uint32_t oset, uint32_t size)
{
    struct process *p = proc_current();
    if (size != 8)
        return -EINVAL;
    uint64_t old = p->sig_blocked;
    if (set) {
        uint64_t s;
        if (copy_from_user(&s, (void *)set, 8) < 0)
            return -EFAULT;
        if (how == SIG_BLOCK)
            p->sig_blocked |= s;
        else if (how == SIG_UNBLOCK)
            p->sig_blocked &= ~s;
        else if (how == SIG_SETMASK)
            p->sig_blocked = s;
        else
            return -EINVAL;
        p->sig_blocked &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    }
    if (oset && copy_to_user((void *)oset, &old, 8) < 0)
        return -EFAULT;
    return 0;
}

int32_t sys_sigreturn_impl(int rt)
{
    struct process *p = proc_current();
    struct thread *t = thread_current();
    struct regs *r = t->uregs;
    struct sigcontext sc;
    uint64_t mask;

    /* The handler's `ret` popped pretcode; the restorer may have popped
       sig too (non-rt). That tells us where the frame starts. */
    if (rt) {
        uint32_t frame = r->user_esp - 4;
        if (copy_from_user(&sc, (void *)(frame + offsetof(struct rt_sigframe, uc.uc_mcontext)),
                           sizeof sc) < 0 ||
            copy_from_user(&mask, (void *)(frame + offsetof(struct rt_sigframe, uc.uc_sigmask)),
                           8) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));
    } else {
        uint32_t frame = r->user_esp - 8, high;
        if (copy_from_user(&sc, (void *)(frame + offsetof(struct sigframe, sc)), sizeof sc) < 0 ||
            copy_from_user(&high, (void *)(frame + offsetof(struct sigframe, extramask)), 4) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));
        mask = ((uint64_t)high << 32) | sc.oldmask;
    }

    /* Put the registers back — but never let a program choose its own
       privilege level (segments) or system flags. */
    r->edi = sc.edi; r->esi = sc.esi; r->ebp = sc.ebp; r->ebx = sc.ebx;
    r->edx = sc.edx; r->ecx = sc.ecx; r->eax = sc.eax;
    r->eip = sc.eip;
    r->user_esp = sc.esp;
    r->eflags = (r->eflags & ~FLAGS_USER) | (sc.eflags & FLAGS_USER);
    r->cs = GDT_USER_CODE;
    r->user_ss = GDT_USER_DATA;
    r->gs = (sc.gs & 3) == 3 ? sc.gs : GDT_USER_DATA;   /* TLS segment */
    r->fs = (sc.fs & 3) == 3 ? sc.fs : GDT_USER_DATA;
    r->ds = r->es = GDT_USER_DATA;
    p->sig_blocked = mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));

    if (sc.fpstate && t->uses_fpu) {
        if (copy_from_user(t->fpu_state, (void *)sc.fpstate, FPU_STATE_SIZE) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));
        uint32_t *mxcsr = (uint32_t *)(t->fpu_state + 24);
        *mxcsr &= 0xFFBF;           /* reserved bits would make fxrstor fault */
        fpu_restore(t->fpu_state);
    }
    return (int32_t)r->eax;         /* the syscall's result = the old EAX */
}

int32_t sys_kill_impl(int pid, int sig)
{
    struct process *me = proc_current();
    if (sig < 0 || sig > NSIG)
        return -EINVAL;
    if (pid > 0) {
        struct process *p = proc_find(pid);
        return p ? signal_send(p, sig) : -ESRCH;
    }
    int pgid = pid == 0 ? me->pgid : -pid;
    if (pid == -1)
        pgid = 0;                       /* everyone (but init) */
    int found = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state != PROC_ALIVE || (pgid && p->pgid != pgid) ||
            (pid == -1 && (p->pid == 1 || p == me)))
            continue;
        signal_send(p, sig);
        found = 1;
    }
    return found ? 0 : -ESRCH;
}

void signal_send_group(int pgid, int sig)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state == PROC_ALIVE && p->pgid == pgid)
            signal_send(p, sig);
    }
}

int32_t sys_rt_sigsuspend_impl(uint32_t set, uint32_t size)
{
    struct process *p = proc_current();
    uint64_t s;
    if (size != 8)
        return -EINVAL;
    if (copy_from_user(&s, (void *)set, 8) < 0)
        return -EFAULT;
    p->sig_saved = p->sig_blocked;      /* restored after the handler */
    p->sig_restore = 1;
    p->sig_blocked = s & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    struct wait_queue never = { 0 };    /* only a signal ends this sleep */
    while (!signal_pending())
        wq_sleep(&never);
    return -EINTR;
}

int32_t sys_rt_sigpending_impl(uint32_t set, uint32_t size)
{
    struct process *p = proc_current();
    if (size != 8)
        return -EINVAL;
    uint64_t s = p->sig_pending & p->sig_blocked;
    return copy_to_user((void *)set, &s, 8);
}
