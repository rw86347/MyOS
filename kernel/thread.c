/*
 * thread.c — kernel threads: create, switch, exit
 *
 * The threads live in a fixed table. thread_switch() calls switch_context()
 * (switch.asm) to jump onto another thread's stack. WHO runs next is decided
 * by sched.c.
 *
 * Preemption: the timer IRQ only sets a flag (sched_tick). The actual switch
 * happens at the very end of interrupt handling (sched_preempt_check) — on
 * the interrupted thread's own stack, where isr.asm already saved all its
 * registers. When we switch back to it later, it finishes the interrupt with
 * `iret` and carries on as if nothing happened.
 */
#include <gdt.h>
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <sched.h>
#include <string.h>
#include <thread.h>

/* In switch.asm */
void switch_context(uint32_t *save_esp, uint32_t new_esp);
void thread_trampoline(void);

static struct thread threads[THREAD_MAX];
static struct thread *current;

static void set_name(struct thread *t, const char *name)
{
    size_t i;
    for (i = 0; name[i] && i < sizeof t->name - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

void thread_init(void)
{
    /* kernel_main is already running on the boot stack from entry.asm.
       It becomes thread 0; its esp gets filled in the first time it yields. */
    current = &threads[0];
    set_name(current, "main");
    current->state = THREAD_READY;
    current->stack = NULL;
}

/* Find a free table slot. A DEAD thread's slot is reused, and its stack
   freed here: a thread can't free its own stack while it's still running on
   it (in thread_exit), but once it's dead and someone else is running, it's
   safe. ("Lazy reaping".) */
static struct thread *alloc_slot(void)
{
    for (int i = 0; i < THREAD_MAX; i++)
        if (threads[i].state == THREAD_UNUSED)
            return &threads[i];
    for (int i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];
        if (t->state == THREAD_DEAD && t != current) {
            kfree(t->stack);
            t->stack = NULL;
            t->state = THREAD_UNUSED;
            return t;
        }
    }
    return NULL;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg)
{
    uint32_t flags = irq_save();        /* the scheduler reads the table too */
    struct thread *t = alloc_slot();
    irq_restore(flags);
    if (!t)
        return NULL;

    t->stack = kmalloc(THREAD_STACK);
    if (!t->stack)
        return NULL;

    /* Build a fake "paused" frame at the top of the new stack, laid out
       exactly as switch_context leaves one (see switch.asm). When someone
       switches to this thread, the pops load EBX = fn and ESI = arg, and
       `ret` jumps to thread_trampoline, which calls fn(arg). */
    uint32_t *sp = (uint32_t *)((char *)t->stack + THREAD_STACK);
    *--sp = (uint32_t)thread_trampoline;  /* EIP for `ret` */
    *--sp = 0;                            /* EBP */
    *--sp = (uint32_t)fn;                 /* EBX */
    *--sp = (uint32_t)arg;                /* ESI */
    *--sp = 0;                            /* EDI */
    t->esp = (uint32_t)sp;

    set_name(t, name);
    t->exit_code = 0;
    t->uses_fpu = 0;
    t->state = THREAD_READY;
    return t;
}

struct thread *thread_next_ready(void)
{
    int me = current - threads;
    for (int i = 1; i < THREAD_MAX; i++) {
        struct thread *t = &threads[(me + i) % THREAD_MAX];
        if (t->state == THREAD_READY)
            return t;
    }
    return NULL;
}

void thread_switch(struct thread *next)
{
    if (!next || next == current)
        return;
    struct thread *prev = current;
    current = next;
    /* If `next` runs a user program, entering the kernel from it must land
       on next's own kernel stack. ("main" never runs user code.) */
    if (next->stack)
        tss_set_kernel_stack((uint32_t)next->stack + THREAD_STACK);
    /* The FPU/SSE registers aren't part of switch_context: save them
       separately, for user threads only (the kernel never uses them). */
    if (prev->uses_fpu)
        fpu_save(prev->fpu_state);
    if (next->uses_fpu)
        fpu_restore(next->fpu_state);
    switch_context(&prev->esp, next->esp);
    /* ...some time later, someone switched back to us. */
}

void thread_yield(void)
{
    /* No interrupts while we pick a thread and switch stacks: a timer tick
       in the middle would try to switch threads from a half-updated state.
       Each thread restores its own interrupt flag when it resumes. */
    uint32_t flags = irq_save();
    thread_switch(sched_pick(1));
    irq_restore(flags);
}

void thread_exit(void)
{
    /* We can't free our own stack — we're standing on it. thread_create()
       frees it later, when it reuses this slot. */
    current->state = THREAD_DEAD;
    thread_yield();
    kprintf("thread_exit: no other thread to run!\n");
    for (;;)
        ;
}

struct thread *thread_current(void)
{
    return current;
}

struct thread *thread_find(const char *name)
{
    for (int i = 0; i < THREAD_MAX; i++)
        if (threads[i].state == THREAD_READY && strcmp(threads[i].name, name) == 0)
            return &threads[i];
    return NULL;
}

void thread_start_fpu(void)
{
    /* Interrupts off: a switch between "load clean registers" and
       "uses_fpu = 1" would not save them, and we'd come back to whatever
       the other thread left in the FPU. */
    uint32_t flags = irq_save();
    fpu_clean_state(current->fpu_state);
    fpu_restore(current->fpu_state);
    current->uses_fpu = 1;
    irq_restore(flags);
}
