#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>

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

/* struct sigaction as the i386 kernel takes it in rt_sigaction(). */
struct k_sigaction {
    uint32_t handler;               /* SIG_DFL, SIG_IGN or a function */
    uint32_t flags;
    uint32_t restorer;              /* the C library's "call sigreturn" code */
    uint64_t mask;                  /* blocked while the handler runs */
} __attribute__((packed));

#define SIGBIT(sig) (1ull << ((sig) - 1))

struct process;
struct regs;

/* Mark `sig` pending for p and wake it if it sleeps. sig 0: just check. */
int  signal_send(struct process *p, int sig);
void signal_send_group(int pgid, int sig);

/* Is a signal waiting for the current process (one it doesn't block)?
   Sleeping syscalls check this and return -EINTR. */
int  signal_pending(void);

/* On the way back to user mode: deliver one pending signal, if any. */
void signal_check(struct regs *r);

/* The fast path for the above: almost always nothing is pending, and then
   this costs one load and one compare — on every syscall and interrupt. */
#define SIGNAL_CHECK(r)                                                     \
    do {                                                                    \
        struct process *p_ = thread_current()->proc;                        \
        if (p_ && p_->sig_pending)                                          \
            signal_check(r);                                                \
    } while (0)

/* fork / execve bookkeeping */
void signal_fork(struct process *child, struct process *parent);
void signal_exec(struct process *p);

/* The system calls. */
int32_t sys_rt_sigaction_impl(int sig, uint32_t act, uint32_t oact, uint32_t size);
int32_t sys_rt_sigprocmask_impl(int how, uint32_t set, uint32_t oset, uint32_t size);
int32_t sys_sigreturn_impl(int rt);
int32_t sys_kill_impl(int pid, int sig);
int32_t sys_rt_sigsuspend_impl(uint32_t set, uint32_t size);
int32_t sys_rt_sigpending_impl(uint32_t set, uint32_t size);

#endif
