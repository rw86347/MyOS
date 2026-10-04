#ifndef PROC_H
#define PROC_H

#include <cred.h>
#include <signal.h>
#include <stdint.h>
#include <thread.h>

/*
 * Processes. A process is a running program: an address space (mm.h), open
 * files, a current directory, a parent, signal handlers, and one or more
 * threads. Its number (pid) is what programs see; pid 1 is the first
 * program (init).
 *
 * Threads (pthreads) are Linux's "thread group": clone(CLONE_THREAD) adds a
 * thread that shares everything above but has its own registers, stack,
 * thread-local storage, signal mask and tid. getpid() is the same in all of
 * them; gettid() differs. exit() ends one thread, exit_group() (and a fatal
 * signal, and execve) all of them. The process ends with its last thread.
 *
 * Life of a process:   fork() or the kernel starts it -> it runs -> exit()
 * or a fatal signal makes it a ZOMBIE (only its exit status is left) -> its
 * parent collects the status with wait4() ("reaping") -> the slot is free.
 */

#define PROC_MAX    64
#define PROC_FILES  32                  /* open files per process (fds 0-31) */

struct file;
struct inode;
struct mm;

enum proc_state { PROC_FREE, PROC_ALIVE, PROC_ZOMBIE };

struct process {
    int pid;
    enum proc_state state;
    struct process *parent;             /* NULL: started by the kernel */
    int kernel_waits;                   /* the kernel will reap it */
    struct thread *threads;             /* its live threads (via proc_next) */
    int nthreads;
    int exiting;                        /* exit_group: the others must end */
    struct wait_queue thread_gone;      /* execve waits here for the others */
    struct mm *mm;
    struct file *files[PROC_FILES];     /* open files, indexed by fd */
    uint32_t cloexec;                   /* bit n: close fd n on execve */
    struct inode *cwd;                  /* current directory */
    uint32_t umask;                     /* permission bits new files lose */
    int status;                         /* wait status, once a zombie */
    struct wait_queue child_wait;       /* sleeps here in wait4() */
    uint32_t start_eip, start_esp;      /* where its program begins */

    /* Process groups and sessions: a shell puts each job (a pipeline) in
       its own group, so Ctrl-C reaches exactly that job. */
    int pgid, sid;

    struct cred cred;                   /* its users and groups (cred.h) */
    int nice;                           /* -20 (most favoured) .. 19; the
                                           scheduler doesn't use it yet */

    /* Signals (signal.c): for the whole process (kill), taken by whichever
       thread doesn't block them. Masks are per thread (thread.h). */
    uint64_t sig_pending;
    struct k_sigaction sig_action[NSIG];

    /* alarm()/setitimer(ITIMER_REAL): SIGALRM at timer_ms() == alarm_at
       (0: not armed), then every alarm_every ms (0: once). Armed processes
       sit on one list sorted by alarm_at, so the tick looks at its head only. */
    uint32_t alarm_at, alarm_every;
    struct process *alarm_next;
};

/* Wait status, encoded as Linux does (what WIFEXITED & co. take apart). */
#define W_EXITED(code)   (((code) & 0xFF) << 8)
#define W_SIGNALED(sig)  ((sig) & 0x7F)
#define W_IFEXITED(s)    (((s) & 0x7F) == 0)
#define W_EXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define W_TERMSIG(s)     ((s) & 0x7F)

struct process *proc_current(void);     /* NULL in a kernel thread */
struct process *proc_find(int pid);     /* a live or zombie process, or NULL */
struct process *proc_at(int i);         /* table slot i (0 <= i < PROC_MAX) */

/* The kernel starts a program (no parent). argv/envp in kernel memory. */
struct process *proc_spawn(const char *path, const char *const argv[],
                           const char *const envp[]);

/* The kernel waits for a program it started; returns its wait status. */
int proc_wait_kernel(struct process *p);

/* Make `p` the process orphans are handed to (init, pid 1). */
void proc_set_init(struct process *p);

/* --- the system calls --- */
int  proc_fork(void);
int  proc_fork_frame(const struct regs *frame, struct process **child);
int  proc_clone(uint32_t flags, uint32_t new_sp, uint32_t parent_tid, uint32_t tls,
                uint32_t child_tid);
int  proc_execve(uint32_t user_path, uint32_t user_argv, uint32_t user_envp);
void proc_exit(int status) __attribute__((noreturn));        /* exit_group */
void proc_thread_exit(int status) __attribute__((noreturn)); /* exit */
int  proc_wait4(int pid, uint32_t user_status, int options);
int  proc_setpgid(int pid, int pgid);
int  proc_setsid(void);
int  proc_pgid_in_session(int pgid, int sid);   /* for TIOCSPGRP */
void proc_set_cwd(struct process *p, struct inode *dir);    /* chdir */

#endif
