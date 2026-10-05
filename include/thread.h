#ifndef THREAD_H
#define THREAD_H

#include <fpu.h>
#include <stdint.h>

/* Threads. A thread runs until it calls thread_yield(), goes to sleep on a
   wait queue, returns, or the timer takes the CPU away. Which thread runs
   next is decided by the scheduler (sched.h); this file is the mechanics.
   A thread that runs a user program belongs to a process (proc.h). */

#define THREAD_MAX      64
#define THREAD_STACK    8192          /* bytes of kernel stack per thread */

struct process;
struct regs;

enum thread_state {
    THREAD_UNUSED,
    THREAD_READY,                     /* running, or may run */
    THREAD_BLOCKED,                   /* asleep on a wait queue */
    THREAD_DEAD,
};

struct mm;

struct thread {
    char name[16];
    enum thread_state state;
    uintptr_t esp;                    /* saved stack pointer while paused
                                         (64 bits on x86_64: switch.asm
                                         stores all of rsp) */
    void *stack;                      /* kmalloc'd stack (NULL for "main") */
    struct process *proc;             /* the process it runs, or NULL */
    struct thread *proc_next;         /* the next thread of the same process */
    int tid;                          /* what gettid() says; the first thread's
                                         tid is its process's pid */
    uint32_t clear_tid;               /* on exit: write 0 here and futex-wake
                                         it (pthread_join waits on it) */
    /* Signals aimed at this thread (tgkill, a fault it caused) and the ones
       it blocks. Signals for the whole process wait in proc->sig_pending
       until a thread that doesn't block them takes one. */
    uint64_t sig_pending, sig_blocked;
    uint64_t sig_saved;               /* sigsuspend: mask to restore */
    int sig_restore;
    /* Non-zero while this thread's or its process's pending set may be
       non-empty: the one word SIGNAL_CHECK reads on every return to user
       mode (Linux's TIF_SIGPENDING). Set by whoever makes a signal
       pending; signal_check() recomputes it. */
    uint32_t sig_check;
    /* futex wait: the word it sleeps on (address space + user address).
       futex_woken says a FUTEX_WAKE ended the sleep (not a signal/timeout). */
    struct mm *futex_mm;
    uint32_t futex_addr;
    uint8_t futex_woken;
    struct mm *mm;                    /* the address space it's using now
                                         (NULL: its process's; a kernel
                                         thread keeps whatever is loaded) */
    struct regs *uregs;               /* user registers saved at the last
                                         syscall (fork copies them) */
    struct wait_queue *wq;            /* the queue it sleeps on (or NULL) */
    struct thread *wq_next;           /* next sleeper on the same wait queue */
    struct thread *rq_next, *rq_prev; /* the run queue: all READY threads */
    uint8_t owns_slot;                /* has slots in the slot table (sched.c) */
    uint8_t is_idle;                  /* the idle thread */
    int uses_fpu;                     /* user thread: save/restore FPU+SSE */
    uint64_t tls[3];                  /* its thread-local storage segments
                                         (GDT entries 6-8; set_thread_area) */
    uint32_t wake_at;                 /* timed sleep: wake at this ms (0: none) */
    struct thread *timer_next;        /* the list of timed sleepers */
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
};

/* A list of threads sleeping until something happens (a child exits, a key
   is pressed, a pipe gets data...). */
struct wait_queue {
    struct thread *first;
};

/* Turn the code that is running now (kernel_main) into thread 0, "main". */
void thread_init(void);

/* Create a thread that will run fn(arg) — for process `proc` (NULL for a
   kernel thread). Returns NULL if out of memory or out of thread slots. It
   starts the next time the scheduler picks it. May reuse the slot of a
   DEAD thread. */
struct thread *thread_create_in(struct process *proc, const char *name,
                                void (*fn)(void *), void *arg);
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

/* fork: a new thread for `proc` that starts by returning to user mode
   with a copy of `frame` (registers saved at the syscall), EAX = 0. */
struct thread *thread_fork(struct process *proc, const char *name,
                           const struct regs *frame);

/* Give up the CPU; the scheduler picks who runs instead. Returns when it's
   our turn again. */
void thread_yield(void);

/* End the current thread. Called automatically when its function returns. */
void thread_exit(void) __attribute__((noreturn));

struct thread *thread_current(void);

/* A small number for the thread (its table slot + 1). */
int thread_id(const struct thread *t);

/* The i-th entry of the thread table (0 <= i < THREAD_MAX). */
struct thread *thread_at(int i);

/* Rename a thread (execve: the new program's name). */
void thread_set_name(struct thread *t, const char *name);

/* Changes whenever a thread is created or exits, so the scheduler knows
   when its cached slot table is stale. */
uint32_t thread_generation(void);

/* The thread with this name, or NULL. (Dead threads are skipped.) */
struct thread *thread_find(const char *name);

/* The live user thread with this tid, or NULL (tkill/tgkill; not a hot
   path: a scan of THREAD_MAX entries). */
struct thread *thread_by_tid(int tid);

/* --- sleeping and waking (interrupts must be off) --- */

/* Sleep until someone calls wq_wake_all() on q. */
void wq_sleep(struct wait_queue *q);

/* Wake every thread sleeping on q. */
void wq_wake_all(struct wait_queue *q);

/* Wake one sleeping thread, whatever it waits for (a signal arrived). */
void thread_wake(struct thread *t);

/* Sleep on q, but at most until timer_ms() reaches `deadline` (0: no
   limit). Returns 1 if the deadline passed, 0 if woken earlier. */
int wq_sleep_until(struct wait_queue *q, uint32_t deadline);

/* Called by the timer every 1 ms: wake the timed sleepers that are due. */
void thread_timer_tick(uint32_t now_ms);

/* --- idle time --- */

/* Halt until an interrupt, counting the time as idle (interrupts must be
   off; they're off again afterwards). */
void cpu_idle(void);
void cpu_idle_end(void);                /* interrupt entry: the CPU woke up */
uint64_t cpu_idle_cycles(void);         /* total so far */

/* --- for the scheduler --- */

/* Round robin: the next READY thread after the current one, or NULL. */
struct thread *thread_next_ready(void);

/* The run queue — a ring of all READY threads (so finding the next one
   doesn't mean scanning the whole thread table). */
struct thread *thread_ready_first(void);
int thread_ready_count(void);

/* Switch the CPU to `next` (no-op if NULL or already running).
   Interrupts must be off. */
void thread_switch(struct thread *next);

/* Saved user GS (set_thread_area): `pop gs` on the way back to user. */
void thread_set_saved_gs(struct thread *t, uint32_t gs);

/* Put t's thread-local storage segments into the GDT (set_thread_area). */
void thread_load_tls(struct thread *t);

/* The current thread is about to run a user program: give it a clean FPU,
   and from now on save/restore the FPU registers when it's switched. */
void thread_start_fpu(void);

#endif
