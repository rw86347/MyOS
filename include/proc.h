/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef PROC_H
#define PROC_H

#include <cred.h>
#include <signal.h>
#include <stdint.h>
#include <user.h>
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

/* Processes in the whole system (a slot is ~2-3 KiB, static). */
#ifdef ARCH_X86_64
#define PROC_MAX    256
#else
#define PROC_MAX    128
#endif
#define PROC_FILES  1024                /* open files per process: Linux's default
                                           RLIMIT_NOFILE (Chromium needs hundreds) */
#define PROC_FILES_BASE 64              /* built into the process; the table grows
                                           past this only for programs that need it */

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
    /* Open files, indexed by fd: files_base, or a bigger table of nfiles
       (doubled from 64 on demand, up to PROC_FILES) — Linux's fdtable. */
    struct file **files;
    uint32_t nfiles;
    uint64_t *cloexec;                  /* bit n: close fd n on execve */
    struct file *files_base[PROC_FILES_BASE];
    uint64_t cloexec_base[PROC_FILES_BASE / 64];
    struct inode *cwd;                  /* current directory */
    struct inode *root;                 /* chroot(): its "/" (NULL: the real one) */
    struct inode *exe;                  /* the ELF execve loaded (/proc/self/exe) */
    uint32_t start_ms;                  /* when it began (/proc/<pid>/stat) */
    uint16_t cmdline_len;               /* bytes of cmdline */
    char cmdline[256];                  /* its argv, NUL after each (/proc/<pid>/cmdline) */
    uint32_t umask;                     /* permission bits new files lose */
    int status;                         /* wait status, once a zombie */
    /* CPU time in ms (see thread.h): of its threads that have ended, and
       of its children it has waited for (with theirs). */
    uint32_t utime, stime, cutime, cstime;
    uint32_t persona;                   /* personality(): 0 = PER_LINUX */
#define ADDR_NO_RANDOMIZE   0x0040000   /* persona: no ASLR (gdb, setarch -R) */
/* Persona bits a setuid / setgid program loses (Linux's PER_CLEAR_ON_SETID:
   MMAP_PAGE_ZERO, ADDR_COMPAT_LAYOUT, READ_IMPLIES_EXEC, and the above). */
#define PER_CLEAR_ON_SETID  0x0740000
    void *sem_undo;                     /* SEM_UNDO records (sem.c): given back at exit */
    const void *lock_wait;              /* F_SETLKW: whose lock it sleeps for (lock.c) */
    struct wait_queue child_wait;       /* sleeps here in wait4() */
    user_addr_t start_eip, start_esp;   /* where its program begins */

    /* Process groups and sessions: a shell puts each job (a pipeline) in
       its own group, so Ctrl-C reaches exactly that job. */
    int pgid, sid;

    /* Job control (signal.c): stopped by SIGSTOP / SIGTSTP / SIGTTIN /
       SIGTTOU until SIGCONT, its threads asleep on stop_wait. What wait4
       hasn't told the parent yet: a stop (its signal), a continue. */
    uint8_t stopped, stop_report, cont_report;
    struct wait_queue stop_wait;

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

    /* POSIX timers (posix_timer.c): how many it owns, and the signals one of
       them has made pending — so delivery only looks for a timer's siginfo
       when there can be one. */
    int ntimers;
    uint64_t timer_sigs;
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
int proc_next_pid(void);                /* the pid the next process will get */

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
int  proc_clone(uint32_t flags, uintptr_t new_sp, uintptr_t parent_tid, uintptr_t tls,
                 uintptr_t child_tid);
int  proc_execve(uintptr_t user_path, uintptr_t user_argv, uintptr_t user_envp);
void proc_exit(int status) __attribute__((noreturn));        /* exit_group */
void proc_thread_exit(int status) __attribute__((noreturn)); /* exit */
int  proc_wait4(int pid, uintptr_t user_status, int options, uintptr_t user_rusage);
int  proc_waitid(int idtype, int id, uintptr_t user_info, int options, uintptr_t user_rusage);
/* Write a struct rusage with these CPU times (ms) to user memory. (sys_info.c) */
int  rusage_to_user(uintptr_t user, uint32_t user_ms, uint32_t sys_ms);
int  proc_setpgid(int pid, int pgid);
int  proc_setsid(void);
/* No member of the group has a parent in another group of its session:
   no shell is there to continue it, so terminal stops don't apply (POSIX). */
int  proc_pgrp_orphaned(int pgid);
/* Does any live process belong to session sid? (A terminal whose session
   has ended is free again, as Linux frees it when the leader exits.) */
int  proc_session_alive(int sid);
int  proc_pgid_in_session(int pgid, int sid);   /* for TIOCSPGRP */
void proc_set_cwd(struct process *p, struct inode *dir);    /* chdir */

#endif
