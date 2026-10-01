#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>

/* Kernel threads. A thread runs until it calls thread_yield(), returns, or
   the timer takes the CPU away. Which thread runs next is decided by the
   scheduler (sched.h); this file is only the mechanics. */

#define THREAD_MAX      16
#define THREAD_STACK    8192          /* bytes of kernel stack per thread */

enum thread_state { THREAD_UNUSED, THREAD_READY, THREAD_DEAD };

struct thread {
    char name[16];
    enum thread_state state;
    uint32_t esp;                     /* saved stack pointer while paused */
    void *stack;                      /* kmalloc'd stack (NULL for "main") */
    int exit_code;                    /* set by exit() or a fatal fault */
};

/* Turn the code that is running now (kernel_main) into thread 0, "main". */
void thread_init(void);

/* Create a thread that will run fn(arg). Returns NULL if out of memory or
   out of thread slots. It starts the next time someone yields to it. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

/* Give up the CPU; the scheduler picks who runs instead. Returns when it's
   our turn again. */
void thread_yield(void);

/* End the current thread. Called automatically when its function returns. */
void thread_exit(void);

struct thread *thread_current(void);

/* The thread with this name, or NULL. (Dead threads are skipped.) */
struct thread *thread_find(const char *name);

/* --- for the scheduler --- */

/* Round robin: the next READY thread after the current one, or NULL. */
struct thread *thread_next_ready(void);

/* Switch the CPU to `next` (no-op if NULL or already running).
   Interrupts must be off. */
void thread_switch(struct thread *next);

#endif
