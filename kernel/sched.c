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
 *       slot:   1    2    3    4    5    6   ...  10  ...  14  ...  19   20
 *       runs:  main idle idle brk  idle radio... main ... brk  ... brk  idle
 *
 * The owner keeps the CPU for its whole slot. If it yields or exits early,
 * the idle thread gets the rest of the slot — no other task may use it, so
 * every task's timing stays exactly what the table says.
 */
#include <interrupts.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <thread.h>

static int slots_on;                /* 0 = round robin, 1 = slot table */
static int preempt_on;              /* round robin: switch on every tick? */
static volatile int slot;           /* slot running now, 1-20 (0 = not started) */
static volatile int need_resched;   /* set by the timer, handled at IRQ exit */
static struct thread *idle;

/* slot_thread[s] = the thread that owns slot s right now (or NULL). Looking
   owners up by name is too slow for every tick, so we keep this table and
   rebuild it only when threads come or go (thread_generation changes).
   Every tick is then one array lookup. */
static struct thread *slot_thread[SCHED_SLOTS + 1];
static uint32_t slot_table_gen = ~0u;

static void rebuild_slot_table(void)
{
    for (int s = 1; s <= SCHED_SLOTS; s++) {
        const char *name = sched_slot_owner(s);
        slot_thread[s] = name ? thread_find(name) : NULL;
    }
    slot_table_gen = thread_generation();
}

/* The idle thread: runs when nobody else may. hlt sleeps until the next
   interrupt, so an idle CPU uses little power. */
static void idle_loop(void *arg)
{
    (void)arg;
    for (;;)
        cpu_wait();
}

void sched_start(void)
{
    idle = thread_create("idle", idle_loop, NULL);

    uint32_t flags = irq_save();
    slot = 1;                       /* the frame starts now, in slot 1, which
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
    if (s == 1 || s == 10)
        return SCHED_SYSTEM_THREAD;
    return slot_config[s];
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
    struct thread *want = slot_thread[slot];

    if (!want || want->state != THREAD_READY)
        want = idle;                /* free slot, or the owner has finished */
    if (yielding && want == thread_current())
        want = idle;                /* owner gave up the rest of its slot */
    return want;
}

void sched_tick(void)
{
    if (slots_on) {
        slot = slot % SCHED_SLOTS + 1;      /* 1, 2, ..., 20, 1, 2, ... */
        need_resched = 1;
    } else if (preempt_on) {
        need_resched = 1;
    }
}

void sched_preempt_check(void)
{
    /* We're inside an interrupt, so interrupts are already off. */
    if (need_resched) {
        need_resched = 0;
        thread_switch(sched_pick(0));
    }
}
