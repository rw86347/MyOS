/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * thread.c — threads: create, switch, sleep, wake, exit
 *
 * The threads live in a fixed table. thread_switch() calls switch_context()
 * (switch.asm) to jump onto another thread's stack — and, if the new thread
 * belongs to a different process, loads that process's address space. WHO
 * runs next is decided by sched.c.
 *
 * Preemption: the timer IRQ only sets a flag (sched_tick). The actual switch
 * happens at the very end of interrupt handling (sched_preempt_check) — on
 * the interrupted thread's own stack, where isr.asm already saved all its
 * registers. When we switch back to it later, it finishes the interrupt with
 * `iret` and carries on as if nothing happened.
 */
#include <cpu.h>
#include <cycles.h>
#include <gdt.h>
#include <interrupts.h>
#include <kprintf.h>
#include <memlayout.h>
#include <mm.h>
#include <paging.h>
#include <pmm.h>
#include <proc.h>
#include <sched.h>
#include <string.h>
#include <thread.h>

/* In switch.asm / isr.asm */
void switch_context(uintptr_t *save_esp, uintptr_t new_esp);
void thread_trampoline(void);
void isr_return(void);

static struct thread threads[THREAD_MAX];
static struct thread *current;
static uint32_t generation;             /* bumped when threads come or go */

/* The run queue: a ring of the READY threads. Every change of state goes
   through set_ready() / set_state(), which keep it right. O(1). */
static struct thread *rq_head;
static int nready;

/* The TLS descriptors now in the GDT: reload only when they differ. */
static uint64_t loaded_tls[3];

#ifdef SYS_TRACE
void thread_trace_dump(struct thread *t, uint32_t n)
{
    uint32_t have = t->trace_at < SYSTRACE_N ? t->trace_at : SYSTRACE_N;
    if (n > have)
        n = have;
    for (uint32_t j = t->trace_at - n; j != t->trace_at; j++) {
        uint32_t k = j % SYSTRACE_N;
        kprintf("  %s/%d %lu ms: sc %lu (%lx, %lx, %lx, %lx) = %ld\n", t->name, t->tid,
                (unsigned long)t->trace[k].ms, (unsigned long)t->trace[k].nr,
                (unsigned long)t->trace[k].a[0], (unsigned long)t->trace[k].a[1],
                (unsigned long)t->trace[k].a[2], (unsigned long)t->trace[k].a[3],
                (long)t->trace[k].ret);
    }
}

void thread_trace_process(struct thread *t)
{
    kprintf("systrace: the last calls of %s/%d:\n", t->name, t->tid);
    thread_trace_dump(t, SYSTRACE_N);
    if (!t->proc)
        return;
    kprintf("systrace: its process's other threads, last 6 each:\n");
    for (struct thread *o = t->proc->threads; o; o = o->proc_next)
        if (o != t)
            thread_trace_dump(o, 6);
}
#endif

void thread_load_tls(struct thread *t)
{
    memcpy(loaded_tls, t->tls, sizeof loaded_tls);
    gdt_load_tls(t->tls);
}

void thread_set_saved_gs(struct thread *t, uint32_t gs)
{
#ifdef ARCH_X86_64
    (void)t;
    (void)gs;
#else
    if (t && t->uregs)
        t->uregs->gs = gs;
#endif
}

static void set_ready(struct thread *t)
{
    if (t->state == THREAD_READY)
        return;
    t->state = THREAD_READY;
    if (!rq_head) {
        t->rq_next = t->rq_prev = t;
        rq_head = t;
    } else {                            /* at the end (just before head) */
        t->rq_next = rq_head;
        t->rq_prev = rq_head->rq_prev;
        rq_head->rq_prev->rq_next = t;
        rq_head->rq_prev = t;
    }
    nready++;
    /* The CPU was idle (the idle thread runs): don't make the newly ready
       thread wait for the next timer tick — switch when this interrupt
       ends. A disk read wakes its thread up to 8 times per 64 KiB; on a
       busy CPU nothing changes (slot owners are never cut short). */
    if (current && current->is_idle)
        sched_need_resched();
}

static void set_state(struct thread *t, enum thread_state s)
{
    if (t->state == THREAD_READY) {     /* leaving the run queue */
        if (t->rq_next == t) {
            rq_head = NULL;
        } else {
            t->rq_prev->rq_next = t->rq_next;
            t->rq_next->rq_prev = t->rq_prev;
            if (rq_head == t)
                rq_head = t->rq_next;
        }
        nready--;
    }
    t->state = s;
}

struct thread *thread_ready_first(void) { return rq_head; }
int thread_ready_count(void) { return nready; }

uint32_t thread_generation(void) { return generation; }
struct thread *thread_current(void) { return current; }
struct thread *thread_at(int i) { return &threads[i]; }
int thread_id(const struct thread *t) { return (int)(t - threads) + 1; }

void thread_set_name(struct thread *t, const char *name)
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
    thread_set_name(current, "main");
    current->stack = NULL;
    set_ready(current);
}

/* Find a free table slot. A DEAD thread's slot is reused once no UNUSED
   one is left (its stack went back to the pool already: stack_get).
   Interrupts off. */
static struct thread *alloc_slot(void)
{
    for (int i = 0; i < THREAD_MAX; i++)
        if (threads[i].state == THREAD_UNUSED)
            return &threads[i];
    for (int i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];
        if (t->state == THREAD_DEAD && t != current && !t->stack) {
            t->state = THREAD_UNUSED;
            return t;
        }
    }
    return NULL;
}

/* --- kernel stacks -----------------------------------------------------------
 *
 * Every thread has a kernel stack of its own: where its system calls run,
 * and the interrupts that arrive while it runs. A stack grows down, and
 * when a call chain goes deeper than the stack, the next push lands on
 * whatever lies below. In a kernel that is the worst kind of bug: silent,
 * and a way in — whoever can make the kernel run off its stack can write
 * kernel memory with it. (It happened: an x86_64 thread overflowed its
 * 8 KiB and wrote over the stack below it, the write-back thread's, which
 * then crashed on a pointer that was really an ext2 inode — docs/LOG.md.)
 *
 * So the stacks are not in the heap, packed together. They live in the
 * KSTACK area (memlayout.h), cut into THREAD_MAX windows, and the bottom
 * page of every window is never mapped — a "guard page":
 *
 *     KSTACK_START + w * KSTACK_WIN:  [ guard page | THREAD_STACK ]
 *
 * Running off the end touches the guard page and the CPU stops right
 * there with a page fault. That fault can't be reported on the stack that
 * is full, so it turns into a double fault, which the CPU takes on a stack
 * of its own (x86_64: IST1; i386: a task gate, gdt.c), and the report
 * names the thread. Linux does the same (CONFIG_VMAP_STACK).
 *
 * The windows are a pool: a free list, last in first out. A new thread
 * takes the window on top — usually the one a thread that just exited gave
 * back, its pages still mapped, so it costs nothing. A window gets pages
 * the first time it is used, and keeps them. So the pages in use are the
 * most threads ever alive at once, not every thread ever made. An exiting
 * thread is still standing on its stack, so it goes on the dead list, and
 * the next new_thread hands its window back. Every step is O(1).
 */
#define KSTACK_WIN ((uintptr_t)THREAD_STACK + PAGE_SIZE)
_Static_assert((uint64_t)THREAD_MAX * KSTACK_WIN <= KSTACK_END - KSTACK_START,
               "a window for every thread");

static uint16_t free_win[THREAD_MAX];   /* the pool: window numbers, top last */
static int nfree_win = -1;              /* -1: not filled in yet */
static uint8_t win_mapped[THREAD_MAX];  /* its pages are there */
static struct thread *win_owner[THREAD_MAX];
static struct thread *dead_list;        /* exited; stacks to hand back */
static uint32_t stack_pages;            /* mapped, in all windows */

#ifdef STACK_DEPTH
#define STACK_PAINT 0x5A                /* untouched stack reads as this */
#endif

static uintptr_t win_base(uint32_t w)   /* the stack's lowest byte */
{
    return KSTACK_START + w * KSTACK_WIN + PAGE_SIZE;
}

/* Put the stacks of threads that have exited (and are off them now) back
   in the pool. Interrupts off. */
static void reap_stacks(void)
{
    struct thread **pp = &dead_list;
    while (*pp) {
        struct thread *t = *pp;
        if (t == current) {             /* still on its way out */
            pp = &t->dead_next;
            continue;
        }
        uint32_t w = (uint32_t)(((uintptr_t)t->stack - KSTACK_START) / KSTACK_WIN);
        win_owner[w] = NULL;
        free_win[nfree_win++] = (uint16_t)w;
        t->stack = NULL;
        *pp = t->dead_next;
        t->dead_next = NULL;
    }
}

/* A window for t: off the pool, mapped if it never was. Interrupts off
   (pmm_alloc doesn't sleep: its reclaim only drops clean cache pages).
   NULL: no window or no RAM. */
static void *stack_get(struct thread *t)
{
    if (nfree_win < 0) {                /* first use: every window, 0 on top */
        for (int k = 0; k < THREAD_MAX; k++)
            free_win[k] = (uint16_t)(THREAD_MAX - 1 - k);
        nfree_win = THREAD_MAX;
    }
    if (!nfree_win)
        return NULL;
    uint32_t w = free_win[--nfree_win];
    uintptr_t base = win_base(w);
    if (!win_mapped[w]) {
        for (uint32_t off = 0; off < THREAD_STACK; off += PAGE_SIZE) {
            uint32_t frame = pmm_alloc();
            if (!frame || vmm_map(base + off, frame, PTE_WRITABLE) < 0) {
                if (frame)
                    pmm_free(frame);
                while (off) {           /* give back what we did get */
                    off -= PAGE_SIZE;
                    pmm_free(vmm_unmap(base + off));
                }
                free_win[nfree_win++] = (uint16_t)w;
                return NULL;
            }
        }
        win_mapped[w] = 1;
        stack_pages += THREAD_STACK / PAGE_SIZE;
    }
    win_owner[w] = t;
    t->stack = (void *)base;
#ifdef STACK_DEPTH
    memset(t->stack, STACK_PAINT, THREAD_STACK);
#endif
    return t->stack;
}

uint32_t thread_stack_pages(void) { return stack_pages; }

void thread_dump(void)
{
    extern char __text_start[], __text_end[];
    static const char *const state[] = { "unused", "ready", "asleep", "dead" };
    uint32_t flags = irq_save();
    kprintf("--- threads (Alt+SysRq+T): slot name pid/tid state [syscall nr, user ip] "
            "[futex] [kernel calls]\n");
    for (int i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];
        if (t->state == THREAD_UNUSED)
            continue;
        kprintf("%3d %-15s %d/%d %s%s", i, t->name, t->proc ? t->proc->pid : 0, t->tid,
                t->state <= THREAD_DEAD ? state[t->state] : "?", t == current ? " (running)" : "");
        if (t->proc && t->uregs && t->state == THREAD_BLOCKED)
#ifdef ARCH_X86_64
            kprintf(" syscall %lu ip %lx", (unsigned long)t->uregs->rax, (unsigned long)t->uregs->rip);
#else
            kprintf(" syscall %lu ip %lx", (unsigned long)t->uregs->eax, (unsigned long)t->uregs->eip);
#endif
        if (t->futex_addr)
            kprintf(" futex %lx", (unsigned long)t->futex_addr);
        /* Where in the kernel it sleeps: the return addresses on its stack,
           newest first (look them up with addr2line). */
        if (t->stack && t != current && t->state == THREAD_BLOCKED) {
            const uintptr_t *p = (const uintptr_t *)t->esp;
            const uintptr_t *top = (const uintptr_t *)((char *)t->stack + THREAD_STACK);
            int n = 0;
            for (; p >= (const uintptr_t *)t->stack && p < top && n < 5; p++)
                if (*p >= (uintptr_t)__text_start && *p < (uintptr_t)__text_end) {
                    kprintf(n++ ? " %lx" : " <- %lx", (unsigned long)*p);
                }
        }
        kprintf("\n");
    }
    irq_restore(flags);
}

struct thread *thread_stack_guard(uintptr_t addr)
{
    if (addr < KSTACK_START || addr - KSTACK_START >= THREAD_MAX * KSTACK_WIN)
        return NULL;
    uintptr_t off = addr - KSTACK_START;
    return off % KSTACK_WIN < PAGE_SIZE ? win_owner[off / KSTACK_WIN] : NULL;
}

#ifdef STACK_DEPTH
/* The deepest any stack has gone: the lowest byte that is no longer the
   paint. A scan of every stack — debug builds only. */
void thread_stack_report(void)
{
    static uint32_t deepest;
    for (int i = 0; i < THREAD_MAX; i++) {
        const uint8_t *s = threads[i].stack;
        if (!s || threads[i].state == THREAD_UNUSED)
            continue;
        uint32_t k = 0;
        while (k < THREAD_STACK && s[k] == STACK_PAINT)
            k++;
        if (THREAD_STACK - k > deepest) {
            deepest = THREAD_STACK - k;
            kprintf("stack: \"%s\" has used %u of its %u bytes\n", threads[i].name,
                    (unsigned)deepest, (unsigned)THREAD_STACK);
        }
    }
}
#endif

/* A new thread whose stack is ready except for the top part, which the
   caller fills in. Not READY yet. */
static struct thread *new_thread(struct process *proc, const char *name)
{
    uint32_t flags = irq_save();        /* the scheduler reads the table too */
    reap_stacks();
    struct thread *t = alloc_slot();
    if (t) {
        t->state = THREAD_BLOCKED;      /* reserved, but not runnable yet */
        if (!stack_get(t)) {
            t->state = THREAD_UNUSED;
            irq_restore(flags);
            kprintf("thread: no stack for %s (%u bytes free)\n", name,
                    (unsigned)pmm_free_count());
            return NULL;
        }
    }
    irq_restore(flags);
    if (!t)
        return NULL;

    thread_set_name(t, name);
    t->proc = proc;
    t->proc_next = NULL;
    t->tid = 0;
    t->clear_tid = 0;
    t->sig_pending = t->sig_blocked = t->sig_saved = 0;
    t->sig_restore = 0;
    t->ss_sp = t->ss_size = 0;
    t->ss_flags = 0;
    t->sig_check = 0;
    t->utime = t->stime = 0;
    t->futex_mm = NULL;
    t->futex_addr = 0;
    t->futex_woken = 0;
    t->mm = NULL;                       /* (slots are reused: no stale one) */
    t->uregs = NULL;
    t->wq = NULL;
    t->wq_next = NULL;
    t->owns_slot = 0;
    t->is_idle = 0;
    t->uses_fpu = 0;
    memset(t->tls, 0, sizeof t->tls);
    t->wake_at = 0;
    t->timer_next = NULL;
    return t;
}

static void start_thread(struct thread *t, void *sp)
{
    t->esp = (uintptr_t)sp;
    uint32_t flags = irq_save();
    set_ready(t);
    generation++;
    irq_restore(flags);
}

struct thread *thread_create_in(struct process *proc, const char *name,
                                void (*fn)(void *), void *arg)
{
    struct thread *t = new_thread(proc, name);
    if (!t)
        return NULL;

    /* Build a fake "paused" frame at the top of the new stack, laid out
       exactly as switch_context leaves one (see switch.asm). When someone
       switches to this thread, the pops load EBX = fn and ESI = arg, and
       `ret` jumps to thread_trampoline, which calls fn(arg). */
#ifdef ARCH_X86_64
    /* switch.asm pops r15 r14 r13 r12 rdi rsi rbx rbp, then `ret`:
       eight registers and the return address, nine words. */
    uint64_t *sp = (uint64_t *)((char *)t->stack + THREAD_STACK);
    *--sp = (uint64_t)(uintptr_t)thread_trampoline;     /* RIP for `ret` */
    *--sp = 0;                                          /* RBP */
    *--sp = (uint64_t)(uintptr_t)fn;                    /* RBX */
    *--sp = (uint64_t)(uintptr_t)arg;                   /* RSI */
    *--sp = 0;                                          /* RDI */
    *--sp = 0;                                          /* R12 */
    *--sp = 0;                                          /* R13 */
    *--sp = 0;                                          /* R14 */
    *--sp = 0;                                          /* R15 */
    start_thread(t, sp);
#else
    uint32_t *sp = (uint32_t *)((char *)t->stack + THREAD_STACK);
    *--sp = (uint32_t)thread_trampoline;  /* EIP for `ret` */
    *--sp = 0;                            /* EBP */
    *--sp = (uint32_t)fn;                 /* EBX */
    *--sp = (uint32_t)arg;                /* ESI */
    *--sp = 0;                            /* EDI */
    start_thread(t, sp);
#endif
    return t;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg)
{
    return thread_create_in(NULL, name, fn, arg);
}

struct thread *thread_fork(struct process *proc, const char *name,
                           const struct regs *frame)
{
    struct thread *t = new_thread(proc, name);
    if (!t)
        return NULL;

    /* At the very top of the stack: a copy of the parent's syscall frame —
       exactly where the CPU puts it when a user program enters the kernel.
       Below it, a paused frame whose `ret` goes to isr_return: the tail of
       the interrupt code, which pops the frame and `iret`s to user mode. */
    uint8_t *top = (uint8_t *)t->stack + THREAD_STACK;
    struct regs *child = (struct regs *)(top - sizeof *child);
    memcpy(child, frame, sizeof *child);
#ifdef ARCH_X86_64
    child->rax = 0;
    uint64_t *sp = (uint64_t *)child;
    *--sp = (uint64_t)(uintptr_t)isr_return;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
#else
    child->eax = 0;                       /* the child sees fork() == 0 */
    uint32_t *sp = (uint32_t *)child;
    *--sp = (uint32_t)isr_return;         /* EIP for `ret` */
    *--sp = 0;                            /* EBP */
    *--sp = 0;                            /* EBX */
    *--sp = 0;                            /* ESI */
    *--sp = 0;                            /* EDI */
#endif

    /* The child continues with the parent's TLS, signal mask and FPU
       registers too. */
    memcpy(t->tls, current->tls, sizeof t->tls);
    t->sig_blocked = current->sig_blocked;
    t->ss_sp = current->ss_sp;          /* fork keeps the alternate signal */
    t->ss_size = current->ss_size;      /* stack (a new thread drops it: */
    t->ss_flags = current->ss_flags;    /* proc.c's clone) */
    if (current->uses_fpu) {
        fpu_save(t->fpu_state);
        t->uses_fpu = 1;
    }
#ifdef ARCH_X86_64
    start_thread(t, sp);
#else
    start_thread(t, sp);
#endif
    return t;
}

struct thread *thread_next_ready(void)
{
    if (!rq_head)
        return NULL;
    struct thread *t = current->state == THREAD_READY ? current->rq_next : rq_head;
    return t == current ? NULL : t;
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
        tss_set_kernel_stack((uintptr_t)next->stack + THREAD_STACK);
    /* Its process's address space. Kernel threads just keep whatever is
       loaded (the kernel half is the same everywhere): no CR3 load, no TLB
       flush when switching to them. */
    if (next->mm)
        mm_switch(next->mm);            /* the one it was using (see mm_switch) */
    else if (next->proc)
        mm_switch(next->proc->mm);
    if (next->proc && memcmp(next->tls, loaded_tls, sizeof loaded_tls) != 0)
        thread_load_tls(next);          /* its thread-local storage */
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

/* --- idle time ---------------------------------------------------------- */

/* Cycles the CPU spent halted, waiting for an interrupt: `top` and
   /proc/stat show it, and benchmarks subtract it to count only work.
   The interrupt that wakes the CPU ends the idle time at once (it may
   switch to another thread before the halted one runs again). */
static uint64_t idle_cycles, idle_since;
static volatile int idling;

void cpu_idle_end(void)                 /* first thing in every interrupt */
{
    if (idling) {
        idling = 0;
        idle_cycles += cycles_now() - idle_since;
    }
}

uint64_t cpu_idle_cycles(void)
{
    uint32_t flags = irq_save();
    uint64_t c = idle_cycles;
    irq_restore(flags);
    return c;
}

void cpu_idle(void)                     /* interrupts off, and off again after */
{
    idle_since = cycles_now();
    idling = 1;
    cpu_sleep();                        /* sti; hlt (or mwait); cli: can't miss an IRQ */
    cpu_idle_end();                     /* (the interrupt normally did it) */
}

void thread_exit(void)
{
    /* We can't give up our own stack — we're standing on it. The next
       new_thread puts it back in the pool (reap_stacks). */
    irq_save();
    set_state(current, THREAD_DEAD);
    if (current->stack) {
        current->dead_next = dead_list;
        dead_list = current;
    }
    generation++;
    for (;;) {
        thread_switch(sched_pick(1));
        cpu_idle();                             /* nobody to run: wait */
    }
}

struct thread *thread_find(const char *name)
{
    for (int i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];
        if ((t->state == THREAD_READY || t->state == THREAD_BLOCKED) &&
            strcmp(t->name, name) == 0)
            return t;
    }
    return NULL;
}

struct thread *thread_by_tid(int tid)
{
    for (int i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];
        if (t->proc && t->tid == tid &&
            (t->state == THREAD_READY || t->state == THREAD_BLOCKED))
            return t;
    }
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

/* --- wait queues ----------------------------------------------------------- */

void wq_sleep(struct wait_queue *q)
{
    set_state(current, THREAD_BLOCKED);
    current->wq = q;
    current->wq_next = q->first;
    q->first = current;
    while (current->state == THREAD_BLOCKED) {
        thread_switch(sched_pick(1));
        /* Back here without having slept means nobody else could run:
           wait for an interrupt (`sti; hlt` can't miss one in between). */
        if (current->state == THREAD_BLOCKED)
            cpu_idle();
    }
}

void wq_wake_all(struct wait_queue *q)
{
    for (struct thread *t = q->first; t; t = t->wq_next) {
        t->wq = NULL;
        if (t->state == THREAD_BLOCKED)
            set_ready(t);
    }
    q->first = NULL;
}

void thread_wake(struct thread *t)
{
    uint32_t flags = irq_save();
    if (t->state == THREAD_BLOCKED && t->wq) {
        /* Take it off its queue first, or the queue would still point
           at it after it sleeps somewhere else. */
        struct thread **pp = &t->wq->first;
        while (*pp && *pp != t)
            pp = &(*pp)->wq_next;
        if (*pp)
            *pp = t->wq_next;
        t->wq = NULL;
        set_ready(t);
    }
    irq_restore(flags);
}

/* --- timed sleeps ------------------------------------------------------------ */

/* Timed sleepers, sorted by wake-up time: each timer tick only looks at
   the first one — O(1) per tick, however many sleep. */
static struct thread *timer_list;

static void timer_remove(struct thread *t)
{
    struct thread **pp = &timer_list;
    while (*pp && *pp != t)
        pp = &(*pp)->timer_next;
    if (*pp)
        *pp = t->timer_next;
    t->wake_at = 0;
}

int wq_sleep_until(struct wait_queue *q, uint32_t deadline)
{
    if (deadline) {
        struct thread **pp = &timer_list;
        while (*pp && (int32_t)((*pp)->wake_at - deadline) <= 0)
            pp = &(*pp)->timer_next;
        current->wake_at = deadline;
        current->timer_next = *pp;
        *pp = current;
    }
    wq_sleep(q);
    int timed_out = deadline && current->wake_at == 0;
    if (current->wake_at)
        timer_remove(current);          /* woken early: leave the list */
    return timed_out;
}

void thread_timer_tick(uint32_t now)
{
    while (timer_list && (int32_t)(now - timer_list->wake_at) >= 0) {
        struct thread *t = timer_list;
        timer_list = t->timer_next;
        t->wake_at = 0;                 /* "the deadline passed" */
        thread_wake(t);
    }
}
