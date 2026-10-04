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
#include <timer.h>
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

/* Would this signal be ignored anyway? Then it's dropped when sent. */
static int ignored(struct process *p, int sig)
{
    uint32_t h = p->sig_action[sig - 1].handler;
    return sig != SIGKILL && (h == SIG_IGN || (h == SIG_DFL && default_ignored(sig)));
}

int signal_send(struct process *p, int sig)
{
    if (sig < 0 || sig > NSIG)
        return -EINVAL;
    if (sig == 0 || ignored(p, sig))
        return 0;                       /* sig 0: "does it exist?" */

    uint32_t flags = irq_save();
    p->sig_pending |= SIGBIT(sig);
    /* Any one thread that doesn't block it may take it; wake that one (its
       sleep ends with -EINTR). If all block it, it waits for one to
       unblock it. A SIGKILL wakes one, which then ends them all. Every
       thread looks on its way out to user mode. */
    struct thread *taker = NULL;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        t->sig_check = 1;
        if (!taker && (sig == SIGKILL || !(t->sig_blocked & SIGBIT(sig))))
            taker = t;
    }
    if (taker && p->state == PROC_ALIVE)
        thread_wake(taker);
    irq_restore(flags);
    return 0;
}

int signal_send_thread(struct thread *t, int sig)
{
    if (sig < 0 || sig > NSIG)
        return -EINVAL;
    if (sig == 0 || ignored(t->proc, sig))
        return 0;
    uint32_t flags = irq_save();
    t->sig_pending |= SIGBIT(sig);
    t->sig_check = 1;
    if (sig == SIGKILL || !(t->sig_blocked & SIGBIT(sig)))
        thread_wake(t);
    irq_restore(flags);
    return 0;
}

int signal_pending(void)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    return p && ((p->sig_pending | t->sig_pending) & ~t->sig_blocked);
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
static int setup_frame(int sig, struct k_sigaction *a, struct regs *r)
{
    struct thread *t = thread_current();
    uint64_t mask = t->sig_restore ? t->sig_saved : t->sig_blocked;
    t->sig_restore = 0;

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
    t->sig_blocked |= a->mask;
    if (!(a->flags & SA_NODEFER))
        t->sig_blocked |= SIGBIT(sig);
    t->sig_blocked &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    if (a->flags & SA_RESETHAND)
        a->handler = SIG_DFL;
    return 0;
}

void signal_check(struct regs *r)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    if (!p || !(r->cs & 3))
        return;
    for (;;) {
        uint64_t pending = t->sig_pending | p->sig_pending;
        t->sig_check = pending != 0;    /* blocked ones: look again later */
        uint64_t ready = pending & ~t->sig_blocked;
        if (pending & SIGBIT(SIGKILL))
            ready |= SIGBIT(SIGKILL);   /* can't be blocked */
        if (!ready) {
            if (t->sig_restore) {       /* sigsuspend ended without a handler */
                t->sig_blocked = t->sig_saved;
                t->sig_restore = 0;
            }
            return;
        }
        int sig = __builtin_ctzll(ready) + 1;   /* lowest number first */
        if (t->sig_pending & SIGBIT(sig))       /* this thread's own first */
            t->sig_pending &= ~SIGBIT(sig);
        else
            p->sig_pending &= ~SIGBIT(sig);

        struct k_sigaction *a = &p->sig_action[sig - 1];
        if (sig == SIGKILL || a->handler == SIG_DFL) {
            if (sig != SIGKILL && default_ignored(sig))
                continue;
            proc_exit(W_SIGNALED(sig));         /* the default: the end */
        }
        if (a->handler == SIG_IGN)
            continue;
        if (setup_frame(sig, a, r) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));     /* no usable stack */
        return;                                 /* one handler at a time */
    }
}

/* --- fork and exec ------------------------------------------------------------- */

void signal_fork(struct process *child, struct process *parent)
{
    /* (The mask is the thread's: thread_fork copies it.) */
    memcpy(child->sig_action, parent->sig_action, sizeof child->sig_action);
    child->sig_pending = 0;             /* pending signals aren't inherited */
    child->alarm_at = child->alarm_every = 0;   /* timers aren't inherited */
    child->alarm_next = NULL;
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
        if (n.handler == SIG_IGN || (n.handler == SIG_DFL && default_ignored(sig))) {
            p->sig_pending &= ~SIGBIT(sig);     /* pending ones are dropped */
            for (struct thread *t = p->threads; t; t = t->proc_next)
                t->sig_pending &= ~SIGBIT(sig);
        }
    }
    return 0;
}

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

int32_t sys_rt_sigprocmask_impl(int how, uint32_t set, uint32_t oset, uint32_t size)
{
    struct thread *t = thread_current();
    if (size != 8)
        return -EINVAL;
    uint64_t old = t->sig_blocked;
    if (set) {
        uint64_t s;
        if (copy_from_user(&s, (void *)set, 8) < 0)
            return -EFAULT;
        if (how == SIG_BLOCK)
            t->sig_blocked |= s;
        else if (how == SIG_UNBLOCK)
            t->sig_blocked &= ~s;
        else if (how == SIG_SETMASK)
            t->sig_blocked = s;
        else
            return -EINVAL;
        t->sig_blocked &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    }
    if (oset && copy_to_user((void *)oset, &old, 8) < 0)
        return -EFAULT;
    return 0;
}

int32_t sys_sigreturn_impl(int rt)
{
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
    /* TLS segments. The ring-3 bits are not enough on their own: the frame is
       the program's to write, and a selector that cannot be loaded faults in
       the kernel's own return path, which is fatal. */
    r->gs = gdt_user_seg_ok(sc.gs) ? sc.gs : GDT_USER_DATA;
    r->fs = gdt_user_seg_ok(sc.fs) ? sc.fs : GDT_USER_DATA;
    r->ds = r->es = GDT_USER_DATA;
    t->sig_blocked = mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));

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
        if (!p) {                       /* a thread's tid names its process */
            struct thread *t = thread_by_tid(pid);
            p = t ? t->proc : NULL;
        }
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

/* Is pgid a live process group in session sid? TIOCSPGRP has to know, because
   the foreground group is who gets the keyboard and SIGINT: handing the
   terminal to a group in someone else's session lets any process take the
   keyboard away from the shell that owns it. A scan of the table, which is
   fine for an ioctl a shell calls once per job. */
int proc_pgid_in_session(int pgid, int sid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state == PROC_ALIVE && p->pgid == pgid && p->sid == sid)
            return 1;
    }
    return 0;
}

int32_t sys_rt_sigsuspend_impl(uint32_t set, uint32_t size)
{
    struct thread *t = thread_current();
    uint64_t s;
    if (size != 8)
        return -EINVAL;
    if (copy_from_user(&s, (void *)set, 8) < 0)
        return -EFAULT;
    t->sig_saved = t->sig_blocked;      /* restored after the handler */
    t->sig_restore = 1;
    t->sig_blocked = s & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    struct wait_queue never = { 0 };    /* only a signal ends this sleep */
    while (!signal_pending())
        wq_sleep(&never);
    return -EINTR;
}

int32_t sys_rt_sigpending_impl(uint32_t set, uint32_t size)
{
    struct thread *t = thread_current();
    if (size != 8)
        return -EINVAL;
    uint64_t s = (t->proc->sig_pending | t->sig_pending) & t->sig_blocked;
    return copy_to_user((void *)set, &s, 8);
}

/* --- alarm and setitimer(ITIMER_REAL) ------------------------------------------ */

static struct process *alarm_list;      /* armed processes, soonest first */

static void alarm_unlink(struct process *p)
{
    for (struct process **pp = &alarm_list; *pp; pp = &(*pp)->alarm_next)
        if (*pp == p) {
            *pp = p->alarm_next;
            break;
        }
    p->alarm_next = NULL;
}

/* Sorted insert: O(armed processes), at most PROC_MAX. */
static void alarm_insert(struct process *p)
{
    struct process **pp = &alarm_list;
    while (*pp && (int32_t)((*pp)->alarm_at - p->alarm_at) <= 0)
        pp = &(*pp)->alarm_next;
    p->alarm_next = *pp;
    *pp = p;
}

void signal_alarm_tick(uint32_t now)
{
    while (alarm_list && (int32_t)(now - alarm_list->alarm_at) >= 0) {
        struct process *p = alarm_list;
        alarm_list = p->alarm_next;
        p->alarm_next = NULL;
        p->alarm_at = 0;
        signal_send(p, SIGALRM);
        if (p->alarm_every) {
            p->alarm_at = now + p->alarm_every;
            if (!p->alarm_at)
                p->alarm_at = 1;        /* 0 means "not armed" */
            alarm_insert(p);
        }
    }
}

void signal_alarm_cancel(struct process *p)
{
    uint32_t flags = irq_save();
    if (p->alarm_at)
        alarm_unlink(p);
    p->alarm_at = p->alarm_every = 0;
    irq_restore(flags);
}

/* Arm (value_ms > 0) or disarm (0); returns the ms that were left. */
static uint32_t alarm_set(struct process *p, uint32_t value_ms, uint32_t every_ms)
{
    uint32_t flags = irq_save();
    uint32_t now = timer_ms(), left = 0;
    if (p->alarm_at) {
        int32_t d = (int32_t)(p->alarm_at - now);
        left = d > 0 ? (uint32_t)d : 1;
        alarm_unlink(p);
    }
    p->alarm_at = 0;
    p->alarm_every = value_ms ? every_ms : 0;
    if (value_ms) {
        p->alarm_at = now + value_ms;
        if (!p->alarm_at)
            p->alarm_at = 1;
        alarm_insert(p);
    }
    irq_restore(flags);
    return left;
}

int32_t sys_alarm_impl(uint32_t seconds)
{
    if (seconds > 0x7FFFFFFFu / 1000)
        seconds = 0x7FFFFFFFu / 1000;
    uint32_t left = alarm_set(proc_current(), seconds * 1000, 0);
    return (int32_t)((left + 999) / 1000);
}

/* struct itimerval on i386: { interval {sec, usec}, value {sec, usec} } */
struct itimerval32 { int32_t i_sec, i_usec, v_sec, v_usec; };

static int tv_to_ms(int32_t sec, int32_t usec, uint32_t *ms)
{
    if (sec < 0 || usec < 0 || usec >= 1000000)
        return -EINVAL;
    if ((uint32_t)sec > 0x7FFFFFFFu / 1000 - 1)
        sec = 0x7FFFFFFF / 1000 - 1;
    *ms = (uint32_t)sec * 1000 + (uint32_t)(usec + 999) / 1000;  /* round up */
    return 0;
}

static void get_itimer(struct process *p, struct itimerval32 *v)
{
    uint32_t flags = irq_save();
    uint32_t left = 0;
    if (p->alarm_at) {
        int32_t d = (int32_t)(p->alarm_at - timer_ms());
        left = d > 0 ? (uint32_t)d : 1;
    }
    v->i_sec = p->alarm_every / 1000;
    v->i_usec = (p->alarm_every % 1000) * 1000;
    v->v_sec = left / 1000;
    v->v_usec = (left % 1000) * 1000;
    irq_restore(flags);
}

int32_t sys_getitimer_impl(int which, uint32_t cur)
{
    if (which != 0)                     /* ITIMER_REAL only */
        return -EINVAL;
    struct itimerval32 v;
    get_itimer(proc_current(), &v);
    return copy_to_user((void *)cur, &v, sizeof v);
}

int32_t sys_setitimer_impl(int which, uint32_t new, uint32_t old)
{
    struct process *p = proc_current();
    struct itimerval32 v, o;
    uint32_t value_ms, every_ms;
    if (which != 0)
        return -EINVAL;
    if (copy_from_user(&v, (void *)new, sizeof v) < 0)
        return -EFAULT;
    if (tv_to_ms(v.v_sec, v.v_usec, &value_ms) < 0 ||
        tv_to_ms(v.i_sec, v.i_usec, &every_ms) < 0)
        return -EINVAL;
    get_itimer(p, &o);
    alarm_set(p, value_ms, every_ms);
    if (old && copy_to_user((void *)old, &o, sizeof o) < 0)
        return -EFAULT;
    return 0;
}
