/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
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
#include <interrupts.h>
#include <kprintf.h>
#include <proc.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <user.h>

/* Signals whose default action is to do nothing (SIGCONT's real work —
   continuing — happens when it is sent, whatever its action). */
static int default_ignored(int sig)
{
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

/* The stop signals: their default action stops the whole process. */
static int stop_sig(int sig)
{
    return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

/* --- job control: stopping and continuing ------------------------------------ */

/* The parent hears of a stop or a continue: SIGCHLD (unless it asked not
   to, SA_NOCLDSTOP), and a wait4 with WUNTRACED / WCONTINUED wakes. */
static void tell_parent(struct process *p)
{
    struct process *pp = p->parent;
    if (!pp || pp->state != PROC_ALIVE)
        return;
    if (!(pp->sig_action[SIGCHLD - 1].flags & SA_NOCLDSTOP))
        signal_send(pp, SIGCHLD);
    wq_wake_all(&pp->child_wait);
}

static void resume(struct process *p, int continued)
{
    if (!p->stopped)
        return;
    p->stopped = 0;
    p->stop_report = 0;
    if (continued) {                    /* (SIGKILL just wakes it, to die) */
        p->cont_report = 1;
        tell_parent(p);
    }
    wq_wake_all(&p->stop_wait);
}

/* Sending a signal does part of job control at once, as on Linux: SIGCONT
   continues a stopped process even if it ignores SIGCONT, and drops any
   stop waiting to be taken; a stop drops a waiting SIGCONT. */
static void job_control_send(struct process *p, int sig)
{
    uint64_t drop = 0;
    if (sig == SIGCONT || sig == SIGKILL)
        resume(p, sig == SIGCONT);
    if (sig == SIGCONT)
        drop = SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU);
    else if (stop_sig(sig))
        drop = SIGBIT(SIGCONT);
    if (drop) {
        p->sig_pending &= ~drop;
        for (struct thread *t = p->threads; t; t = t->proc_next)
            t->sig_pending &= ~drop;
    }
}

/* Taken with the default action: stop, until SIGCONT (or SIGKILL). The
   other threads stop when they next head back to user mode. */
static void do_stop(struct thread *t, struct process *p, int sig)
{
    p->stopped = 1;
    p->stop_report = (uint8_t)sig;
    p->cont_report = 0;
    tell_parent(p);
    for (struct thread *o = p->threads; o; o = o->proc_next)
        if (o != t)
            o->sig_check = 1;
}

static void sleep_while_stopped(struct thread *t, struct process *p)
{
    while (p->stopped && !((t->sig_pending | p->sig_pending) & SIGBIT(SIGKILL)))
        wq_sleep(&p->stop_wait);
}

/* --- sending ---------------------------------------------------------------- */

/* Would this signal be ignored anyway? Then it's dropped when sent. */
static int ignored(struct process *p, int sig)
{
    user_addr_t h = p->sig_action[sig - 1].handler;
    return sig != SIGKILL && (h == SIG_IGN || (h == SIG_DFL && default_ignored(sig)));
}

int signal_send(struct process *p, int sig)
{
    if (sig < 0 || sig > NSIG)
        return -EINVAL;
    if (sig)
        job_control_send(p, sig);
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
    if (sig)
        job_control_send(t->proc, sig);  /* stops are the whole process's */
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

int signal_ignored_or_blocked(int sig)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    return !p || (t->sig_blocked & SIGBIT(sig)) || p->sig_action[sig - 1].handler == SIG_IGN;
}

int signal_pending(void)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    return p && ((p->sig_pending | t->sig_pending) & ~t->sig_blocked);
}

/* --- delivering --------------------------------------------------------------- */

/* Run handler a for sig: the CPU's code builds the frame (arch/<cpu>/signal.c),
   then the handler's mask applies. 0, or -1 if the frame can't be written. */
static int setup_frame(int sig, const struct sig_origin *o, struct k_sigaction *a,
                       struct regs *r)
{
    struct thread *t = thread_current();
    /* sigsuspend: the mask to restore after the handler is the one from
       before the call, not the one it waited with. */
    uint64_t mask = t->sig_restore ? t->sig_saved : t->sig_blocked;
    t->sig_restore = 0;
    if (arch_signal_frame(sig, o, a, mask, r) < 0)
        return -1;

    /* While the handler runs, its mask (and the signal itself) are blocked. */
    t->sig_blocked |= a->mask;
    if (!(a->flags & SA_NODEFER))
        t->sig_blocked |= SIGBIT(sig);
    t->sig_blocked &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    if (a->flags & SA_RESETHAND)
        a->handler = SIG_DFL;
    return 0;
}

/* Take sig off the pending sets — the thread's own first — and say where
   it came from. Interrupts off. */
static void take(struct thread *t, int sig, struct sig_origin *o)
{
    struct process *p = t->proc;
    uint64_t bit = SIGBIT(sig);
    int own = (t->sig_pending & bit) != 0;
    if (own)
        t->sig_pending &= ~bit;
    else
        p->sig_pending &= ~bit;
    o->code = SI_USER;
    o->timer_id = o->overrun = 0;
    o->value = 0;
    if (p->timer_sigs & bit)            /* one load when no timer is involved */
        ptimer_siginfo(p, t, sig, own, o);
}

void signal_restart_note(uint32_t nr)
{
    struct thread *t = thread_current();
    t->restart_nr = nr + 1;
    t->sig_check = 1;                   /* signal_check decides, even if the
                                           signal went to a sibling meanwhile */
}

/*
 * A syscall cut short by a signal (-ERESTARTSYS), as Linux treats it:
 *   - a handler runs that has SA_RESTART: the call runs again after it;
 *   - a handler runs without: the call says EINTR;
 *   - no handler runs (the signal was ignored, or blocked again): the call
 *     runs again, and the program never notices.
 * Calls that are never restarted (poll, nanosleep...) return -EINTR
 * themselves, as on Linux.
 */
static void restart_or_eintr(struct thread *t, struct regs *r, const struct k_sigaction *a)
{
    uint32_t nr = t->restart_nr;
    if (!nr)
        return;
    t->restart_nr = 0;
    if (!a || (a->flags & SA_RESTART))
        arch_syscall_restart(r, nr - 1);
    else
        arch_syscall_result(r, -EINTR);
}

void signal_check(struct regs *r)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    if (!p || !(r->cs & 3))
        return;
    sleep_while_stopped(t, p);          /* a sibling stopped the process */
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
            restart_or_eintr(t, r, NULL);   /* no handler ran: again */
            return;
        }
        int sig = __builtin_ctzll(ready) + 1;   /* lowest number first */
        struct sig_origin o;
        take(t, sig, &o);

        struct k_sigaction *a = &p->sig_action[sig - 1];
        if (sig == SIGKILL || a->handler == SIG_DFL) {
            if (stop_sig(sig)) {
                /* A group no shell can continue isn't stopped by the
                   terminal's signals (SIGSTOP always stops). */
                if (sig == SIGSTOP || !proc_pgrp_orphaned(p->pgid)) {
                    do_stop(t, p, sig);
                    sleep_while_stopped(t, p);
                }
                continue;
            }
            if (sig != SIGKILL && default_ignored(sig))
                continue;
            proc_exit(W_SIGNALED(sig));         /* the default: the end */
        }
        if (a->handler == SIG_IGN)
            continue;
        restart_or_eintr(t, r, a);      /* before the frame saves the registers */
        if (setup_frame(sig, &o, a, r) < 0)
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
    child->ntimers = 0;
    child->timer_sigs = 0;
}

void signal_exec(struct process *p)
{
    ptimer_release_all(p);              /* timers don't survive execve */
    /* The handlers were in the old program: gone. Ignored stays ignored. */
    for (int i = 0; i < NSIG; i++) {
        if (p->sig_action[i].handler != SIG_IGN)
            p->sig_action[i].handler = SIG_DFL;
        p->sig_action[i].flags = 0;
        p->sig_action[i].mask = 0;
    }
}

/* --- system calls --------------------------------------------------------------- */

/* --- the alternate signal stack (sigaltstack) ---------------------------------- */

/*
 * A thread may name a second stack for signal handlers (sigaltstack), and a
 * handler installed with SA_ONSTACK runs there. That is how a program
 * reports a stack overflow: the overflowing stack has no room left for the
 * handler's frame. Crash reporters (Chromium's crashpad), GNU grep and Go's
 * runtime all set one up.
 */
#define MINSIGSTKSZ 2048

struct user_stack {                     /* stack_t, the CPU's natural layout */
    user_addr_t sp;
    int32_t flags;
    user_addr_t size;
};

/* Is sp on t's alternate stack? (Linux's on_sig_stack: sp points just past
   what it pushed, so the top itself counts. A disarmed stack never does.) */
static int on_altstack(const struct thread *t, uintptr_t sp)
{
    if (t->ss_flags & SS_AUTODISARM)
        return 0;
    return t->ss_size && sp > t->ss_sp && sp - t->ss_sp <= t->ss_size;
}

static int32_t altstack_flags(const struct thread *t, uintptr_t sp)
{
    if (!t->ss_size)
        return SS_DISABLE;
    return (int32_t)((on_altstack(t, sp) ? SS_ONSTACK : 0) | t->ss_flags);
}

/* Change t's alternate stack (sp: where the thread's user stack is now). */
static int set_altstack(struct thread *t, uintptr_t sp, const struct user_stack *ss)
{
    if (on_altstack(t, sp))
        return -EPERM;                  /* not while running on it */
    uint32_t mode = (uint32_t)ss->flags & ~SS_AUTODISARM;
    if (mode == SS_DISABLE) {
        t->ss_sp = t->ss_size = 0;
        t->ss_flags = 0;
        return 0;
    }
    if (mode != 0 && mode != SS_ONSTACK) /* (SS_ONSTACK: old programs pass it) */
        return -EINVAL;
    if (ss->size < MINSIGSTKSZ)
        return -ENOMEM;
    if (ss->sp >= USER_ADDR_END || ss->size > USER_ADDR_END - ss->sp)
        return -EFAULT;
    t->ss_sp = ss->sp;
    t->ss_size = ss->size;
    t->ss_flags = (uint32_t)ss->flags & SS_AUTODISARM;
    return 0;
}

/* int sigaltstack(const stack_t *ss, stack_t *old_ss) */
SYSCALL_DEF(sys_sigaltstack)
{
    struct thread *t = thread_current();
    uintptr_t sp = t->uregs ? regs_sp(t->uregs) : 0;
    struct user_stack old = { t->ss_sp, altstack_flags(t, sp), t->ss_size };
    if (a1) {
        struct user_stack ss;
        if (copy_from_user(&ss, (const void *)a1, sizeof ss) < 0)
            return -EFAULT;
        int err = set_altstack(t, sp, &ss);
        if (err)
            return err;
    }
    if (a2 && copy_to_user((void *)a2, &old, sizeof old) < 0)
        return -EFAULT;
    return 0;
}

uintptr_t sig_frame_top(const struct k_sigaction *a, uintptr_t sp, int *alt)
{
    struct thread *t = thread_current();
    *alt = (a->flags & SA_ONSTACK) && t->ss_size && !on_altstack(t, sp);
    return *alt ? t->ss_sp + t->ss_size : sp;
}

void sig_save_altstack(uintptr_t sp, user_addr_t *ss_sp, int32_t *ss_flags, user_addr_t *ss_size)
{
    struct thread *t = thread_current();
    *ss_sp = t->ss_sp;
    *ss_flags = altstack_flags(t, sp);
    *ss_size = t->ss_size;
    if (t->ss_flags & SS_AUTODISARM) {  /* the handler may set up another */
        t->ss_sp = t->ss_size = 0;
        t->ss_flags = 0;
    }
}

void sig_restore_altstack(user_addr_t ss_sp, int32_t ss_flags, user_addr_t ss_size)
{
    struct thread *t = thread_current();
    struct user_stack ss = { ss_sp, ss_flags & ~SS_ONSTACK, ss_size };
    set_altstack(t, t->uregs ? regs_sp(t->uregs) : 0, &ss);    /* (as Linux: errors ignored) */
}

int32_t sys_rt_sigaction_impl(int sig, uintptr_t act, uintptr_t oact, uint32_t size)
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

int32_t sys_rt_sigprocmask_impl(int how, uintptr_t set, uintptr_t oset, uint32_t size)
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

/* May the calling program signal p? Linux's rule (kill_ok_by_cred): root
   may signal anyone; anyone else, a process whose real or saved user is
   the caller's real or effective user — your own programs, including a
   setuid one you started, but nobody else's. SIGCONT may also go to any
   process in the caller's session, so a shell can continue a job that
   changed user (su). Signals the kernel sends itself (Ctrl-C, SIGCHLD,
   SIGPIPE) don't come through here. Signal 0 is checked too: "may I?" is
   what `kill -0` asks. */
int signal_allowed(const struct process *p, int sig)
{
    const struct process *me = proc_current();
    const struct cred *c = &me->cred, *t = &p->cred;
    if (c->euid == 0 || p == me)
        return 1;
    if (c->euid == t->uid || c->euid == t->suid || c->uid == t->uid || c->uid == t->suid)
        return 1;
    return sig == SIGCONT && p->sid == me->sid;
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
        if (!p)
            return -ESRCH;
        return signal_allowed(p, sig) ? signal_send(p, sig) : -EPERM;
    }
    int pgid = pid == 0 ? me->pgid : -pid;
    if (pid == -1)
        pgid = 0;                       /* everyone (but init) */
    int found = 0, sent = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state != PROC_ALIVE || (pgid && p->pgid != pgid) ||
            (pid == -1 && (p->pid == 1 || p == me)))
            continue;
        found = 1;
        if (signal_allowed(p, sig)) {
            signal_send(p, sig);
            sent = 1;
        }
    }
    /* Linux's answers: a group with nobody we may signal is EPERM; for
       "everyone" (-1) it is ESRCH, as if those processes weren't there. */
    if (sent)
        return 0;
    return found && pid != -1 ? -EPERM : -ESRCH;
}

void signal_send_group(int pgid, int sig)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state == PROC_ALIVE && p->pgid == pgid)
            signal_send(p, sig);
    }
}

int32_t sys_rt_sigsuspend_impl(uintptr_t set, uint32_t size)
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

/* pause(): sleep until a signal comes that isn't blocked or ignored. Its
   handler runs on the way out, then pause returns -EINTR, as on Linux. */
int32_t sys_pause_impl(void)
{
    struct wait_queue never = { 0 };    /* only a signal ends this sleep */
    while (!signal_pending())
        wq_sleep(&never);
    return -EINTR;
}

int32_t sys_rt_sigpending_impl(uintptr_t set, uint32_t size)
{
    struct thread *t = thread_current();
    if (size != 8)
        return -EINVAL;
    uint64_t s = (t->proc->sig_pending | t->sig_pending) & t->sig_blocked;
    return copy_to_user((void *)set, &s, 8);
}

/* sigwaitinfo / sigtimedwait: take a signal from `set` — normally ones the
   thread blocks — without running a handler, and return its number. While
   it waits, the set is unblocked so that a sender wakes it; any other
   signal it doesn't block ends the wait with -EINTR. musl's SIGEV_THREAD
   timers wait here for their signal. */
int32_t sys_rt_sigtimedwait_impl(uintptr_t uset, uintptr_t uinfo, uintptr_t utimeout,
                                 uint32_t size, int wide)
{
    struct thread *t = thread_current();
    struct process *p = t->proc;
    uint64_t want;
    uint32_t deadline = 0;
    int poll = 0;
    if (size != 8)
        return -EINVAL;
    if (copy_from_user(&want, (void *)uset, 8) < 0)
        return -EFAULT;
    want &= ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    if (utimeout) {
        int64_t ts[2];
        if (wide) {
            if (copy_from_user(ts, (void *)utimeout, sizeof ts) < 0)
                return -EFAULT;
        } else {
            int32_t ts32[2];
            if (copy_from_user(ts32, (void *)utimeout, sizeof ts32) < 0)
                return -EFAULT;
            ts[0] = ts32[0];
            ts[1] = ts32[1];
        }
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000)
            return -EINVAL;
        if (ts[0] > 0x7FFFFFFF / 1000 - 1)
            ts[0] = 0x7FFFFFFF / 1000 - 1;
        uint32_t ms = (uint32_t)ts[0] * 1000 + (uint32_t)(ts[1] + 999999) / 1000000;
        poll = ms == 0;
        deadline = timer_ms() + ms + 1;         /* now is partway through a ms */
        if (!deadline)
            deadline = 1;                       /* 0 means "no limit" */
    }

    uint64_t saved = t->sig_blocked;
    int sig = 0, timed_out = 0;
    for (;;) {
        uint64_t ready = (t->sig_pending | p->sig_pending) & want;
        if (ready) {
            sig = __builtin_ctzll(ready) + 1;
            break;
        }
        if (signal_pending() || poll || timed_out)
            break;
        t->sig_blocked = saved & ~want;
        timed_out = wq_sleep_until(&(struct wait_queue){ 0 }, deadline);
        t->sig_blocked = saved;
    }
    if (!sig)
        return poll || timed_out ? -EAGAIN : -EINTR;

    struct sig_origin o;
    take(t, sig, &o);
    if (uinfo) {
        uint8_t si[SIGINFO_SIZE];
        arch_siginfo(si, sig, &o);
        if (copy_to_user((void *)uinfo, si, sizeof si) < 0)
            return -EFAULT;
    }
    return sig;
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

/* struct itimerval: { interval {sec, usec}, value {sec, usec} }, longs as in
   Linux: 32 bits on i386, 64 on x86_64. */
struct itimerval_k { long i_sec, i_usec, v_sec, v_usec; };

static int tv_to_ms(long sec, long usec, uint32_t *ms)
{
    if (sec < 0 || usec < 0 || usec >= 1000000)
        return -EINVAL;
    if ((unsigned long)sec > 0x7FFFFFFFu / 1000 - 1)
        sec = 0x7FFFFFFF / 1000 - 1;
    *ms = (uint32_t)sec * 1000 + (uint32_t)(usec + 999) / 1000;  /* round up */
    return 0;
}

static void get_itimer(struct process *p, struct itimerval_k *v)
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

int32_t sys_getitimer_impl(int which, uintptr_t cur)
{
    if (which != 0)                     /* ITIMER_REAL only */
        return -EINVAL;
    struct itimerval_k v;
    get_itimer(proc_current(), &v);
    return copy_to_user((void *)cur, &v, sizeof v);
}

int32_t sys_setitimer_impl(int which, uintptr_t new, uintptr_t old)
{
    struct process *p = proc_current();
    struct itimerval_k v, o;
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
