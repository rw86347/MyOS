#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include <user.h>

/*
 * Signals: how Unix interrupts a program. Ctrl-C sends SIGINT, a child's
 * exit SIGCHLD to its parent, a bad pointer SIGSEGV, kill() any of them.
 * Each signal is either ignored, handled by the program's own function
 * (sigaction), or takes its default action — usually ending the program.
 *
 * Signals are delivered on the way back to user mode (signal_check): the
 * kernel builds a frame on the program's stack and "returns" into the
 * handler; when the handler returns, its last step is the sigreturn
 * syscall, which puts everything back as it was.
 */

#define NSIG 64                     /* signals 1..64 (Linux: 1-31 + real-time) */

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG  23
#define SIGWINCH 28

#define SIG_DFL 0
#define SIG_IGN 1

/* sa_flags */
#define SA_NOCLDSTOP 0x00000001
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* struct sigaction as the kernel takes it in rt_sigaction(): 20 bytes on
   i386, 32 on x86_64 (where flags is an unsigned long too). */
struct k_sigaction {
    user_addr_t handler;            /* SIG_DFL, SIG_IGN or a function */
    user_addr_t flags;
    user_addr_t restorer;           /* the C library's "call sigreturn" code */
    uint64_t mask;                  /* blocked while the handler runs */
} __attribute__((packed));

#define SIGBIT(sig) (1ull << ((sig) - 1))

struct process;
struct regs;
struct thread;

/* Mark `sig` pending for the whole process p and wake one of its threads
   that doesn't block it. sig 0: just check. */
int  signal_send(struct process *p, int sig);
void signal_send_group(int pgid, int sig);

/* The same for one thread (tgkill, a fault it caused). */
int  signal_send_thread(struct thread *t, int sig);

/* Is a signal waiting for the current thread (one it doesn't block)?
   Sleeping syscalls check this and return -EINTR. */
int  signal_pending(void);

/* On the way back to user mode: deliver one pending signal, if any. */
void signal_check(struct regs *r);

/* The fast path for the above: almost always nothing is pending, and then
   this costs one load and one compare — on every syscall and interrupt.
   (Kernel threads never get sig_check set.) */
#define SIGNAL_CHECK(r)                                                     \
    do {                                                                    \
        if (thread_current()->sig_check)                                    \
            signal_check(r);                                                \
    } while (0)

/* fork / execve bookkeeping */
void signal_fork(struct process *child, struct process *parent);
void signal_exec(struct process *p);

/* The system calls. */
int32_t sys_rt_sigaction_impl(int sig, uintptr_t act, uintptr_t oact, uint32_t size);
int32_t sys_rt_sigprocmask_impl(int how, uintptr_t set, uintptr_t oset, uint32_t size);
int32_t sys_alarm_impl(uint32_t seconds);
int32_t sys_getitimer_impl(int which, uintptr_t cur);
int32_t sys_setitimer_impl(int which, uintptr_t new, uintptr_t old);

/* Timer tick: send SIGALRM to processes whose alarm is due. */
void signal_alarm_tick(uint32_t now);
void signal_alarm_cancel(struct process *p);   /* on exit */
intptr_t sys_sigreturn_impl(int rt);            /* (arch/<cpu>/signal.c) */
int32_t sys_kill_impl(int pid, int sig);
int32_t sys_rt_sigsuspend_impl(uintptr_t set, uint32_t size);
int32_t sys_pause_impl(void);
int32_t sys_rt_sigpending_impl(uintptr_t set, uint32_t size);
int32_t sys_rt_sigtimedwait_impl(uintptr_t set, uintptr_t info, uintptr_t timeout,
                                 uint32_t size, int wide);

/* --- POSIX timers (posix_timer.c) --- */

#define SI_USER   0
#define SI_TIMER (-2)

/* Where a delivered signal came from: si_code, and for SI_TIMER the timer's
   id, its overrun count and its sigev_value (a pointer-sized word). */
struct sig_origin {
    int32_t code;
    int32_t timer_id, overrun;
    user_addr_t value;
};

/* --- the CPU-specific part (arch/<cpu>/signal.c) --- */

/* Build the signal frame for handler a on the program's stack, as Linux lays
   it out on this CPU (the C library's code depends on it), and point the
   registers r at the handler. `mask` is what sigreturn will restore. 0, or -1
   if the frame can't be written (the program then dies with SIGSEGV). */
int  arch_signal_frame(int sig, const struct sig_origin *o, const struct k_sigaction *a,
                       uint64_t mask, struct regs *r);

/* The alternate signal stack (sigaltstack), for the frame code:
   sig_frame_top: where handler a's frame starts, given the stack pointer sp
     the program was interrupted at (the alternate stack's top if a asks for
     it and the thread isn't on it already; *alt says so).
   sig_save_altstack: the settings to keep in the frame's uc_stack (Linux's
     __save_altstack), then disarm an SS_AUTODISARM stack for the handler.
   sig_restore_altstack: sigreturn puts uc_stack's settings back. */
#define SS_ONSTACK    1
#define SS_DISABLE    2
#define SS_AUTODISARM (1u << 31)
uintptr_t sig_frame_top(const struct k_sigaction *a, uintptr_t sp, int *alt);
void sig_save_altstack(uintptr_t sp, user_addr_t *ss_sp, int32_t *ss_flags, user_addr_t *ss_size);
void sig_restore_altstack(user_addr_t ss_sp, int32_t ss_flags, user_addr_t ss_size);

/* A siginfo_t (128 bytes) as this CPU's programs read it (sigtimedwait). */
#define SIGINFO_SIZE 128
void arch_siginfo(void *si, int sig, const struct sig_origin *o);

/* Signal sig was just taken from thread t's own pending set (own = 1) or its
   process's. If a timer made it pending, fill *o and return 1. Interrupts off. */
int  ptimer_siginfo(struct process *p, struct thread *t, int sig, int own,
                    struct sig_origin *o);
void ptimer_tick(uint32_t now);                         /* every 1 ms */
void ptimer_thread_gone(struct process *p, struct thread *t);
void ptimer_release_all(struct process *p);             /* exit and execve */

#endif
