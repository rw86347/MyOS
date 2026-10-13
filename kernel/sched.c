/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * sched.c — the scheduler: decides WHICH thread runs. (thread.c does the
 * switching itself.)
 *
 * Two modes:
 *   round robin  during boot: every thread takes turns. With preemption on,
 *                the timer forces a turn every tick.
 *   slots        after sched_start(): every 1 ms tick moves to the next slot
 *                of the 20-slot frame and runs that slot's owner:
 *
 *       slot:   0    1    2    3    4    5    6   ...  10  ...  14  ...  19
 *       runs:  main idle idle idle brk  idle radio... main ... brk  ... brk
 *
 * Slots 0 and 10 are the OS's, exactly 10 ms apart; the owners of the
 * others come from the kernel config (kernel/kconfig.c: built in from
 * config/slots.conf, or what Settings › Kernel saved).
 *
 * The owner keeps the CPU for its whole slot; its timing is exactly what
 * the table says, whatever else is running.
 *
 * Everything else — ordinary programs, the shell, kernel helper threads —
 * is "best effort": those threads share the leftover time round robin,
 * 1 ms at a time. Leftover time is a free slot, or a slot whose owner is
 * asleep, has finished, or gave the rest of its slot away (yield). Only if
 * no best-effort thread is ready does the idle thread run (hlt).
 */
#include <interrupts.h>
#include <kconfig.h>
#include <kprintf.h>
#include <pmm.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <thread.h>

static int slots_on;                /* 0 = round robin, 1 = slot table */
static int preempt_on;              /* round robin: switch on every tick? */
static volatile int slot = -1;      /* slot running now, 0-19 (-1 = not started) */
static volatile int need_resched;   /* set by the timer, handled at IRQ exit */
static struct thread *idle;

/* slot_thread[s] = the thread that owns slot s right now (or NULL). Looking
   owners up by name is too slow for every tick, so we keep this table and
   rebuild it only when threads come or go (thread_generation changes).
   Every tick is then one array lookup. */
static struct thread *slot_thread[SCHED_SLOTS];
static uint32_t slot_table_gen = ~0u;
static struct thread *be_last;      /* best effort: the last pick */

static void rebuild_slot_table(void)
{
    for (int i = 0; i < THREAD_MAX; i++)
        thread_at(i)->owns_slot = 0;
    for (int s = 0; s < SCHED_SLOTS; s++) {
        const char *name = sched_slot_owner(s);
        slot_thread[s] = name ? thread_find(name) : NULL;
        if (slot_thread[s])
            slot_thread[s]->owns_slot = 1;
    }
    slot_table_gen = thread_generation();
}

/* The next ready best-effort thread after the last one picked (round
   robin), or NULL. Walks the run queue (READY threads only), so it's
   short: slot owners and idle are the only ones skipped. */
static struct thread *next_best_effort(int skip_current)
{
    int n = thread_ready_count();
    struct thread *t = (be_last && be_last->state == THREAD_READY)
                     ? be_last->rq_next : thread_ready_first();
    for (int i = 0; i < n; i++, t = t->rq_next) {
        if (!t->owns_slot && !t->is_idle &&
            !(skip_current && t == thread_current())) {
            be_last = t;
            return t;
        }
    }
    return NULL;
}

/* The idle thread: runs when nobody else may. hlt sleeps until the next
   interrupt, so an idle CPU uses little power. */
static void idle_loop(void *arg)
{
    (void)arg;
    irq_save();
    for (;;)
        cpu_idle();                 /* counted as idle time (thread.c) */
}

void sched_start(void)
{
    idle = thread_create("idle", idle_loop, NULL);
    if (!idle) {
        kprintf("sched: idle thread failed (%u frames free)\n",
                (unsigned)pmm_free_count());
        return;
    }
    idle->is_idle = 1;

    uint32_t flags = irq_save();
    slot = 0;                       /* the frame starts now, in slot 0, which
                                       belongs to main — the thread running */
    slots_on = 1;
    irq_restore(flags);
}

int sched_slot(void)
{
    return slot;
}

const char *sched_slot_owner(int s)
{
    return kconfig_slot_owner(s);   /* "main" for 0 and 10 */
}

void sched_set_preempt(int on)
{
    preempt_on = on;
}

struct thread *sched_pick(int yielding)
{
    if (!slots_on)
        return thread_next_ready();

    if (slot_table_gen != thread_generation())
        rebuild_slot_table();               /* rare: a thread came or went */
    struct thread *owner = slot_thread[slot];
    if (owner && owner->state == THREAD_READY &&
        !(yielding && owner == thread_current()))
        return owner;                       /* the slot's owner, on time */

    /* Leftover time: best-effort threads take turns. */
    struct thread *t = next_best_effort(yielding);
    return t ? t : idle;
}

void sched_tick(void)
{
    if (slots_on) {
        slot = slot == SCHED_SLOTS - 1 ? 0 : slot + 1;  /* 0, 1, ..., 19, 0, ... */
        need_resched = 1;
    } else if (preempt_on) {
        need_resched = 1;
    }
}

void sched_need_resched(void)
{
    need_resched = 1;
}

void sched_preempt_check(void)
{
    /* We're inside an interrupt, so interrupts are already off. */
    if (need_resched) {
        need_resched = 0;
        thread_switch(sched_pick(0));
    }
}
