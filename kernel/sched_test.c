/*
 * sched_test.c — start the slot scheduler, and prove each task runs in
 * exactly the slots config/slots.conf gives it.
 *
 * One thread is created for every name in the config. Each one loops:
 * "note which slot I'm in, then sleep until the next interrupt". main does
 * the same while it waits. After 200 ms (10 full frames) we print where
 * everyone ran, e.g.
 *     brakes ran in slots: 4 9 14 19
 * and check it against the table. The task threads keep running afterwards.
 */
#include <interrupts.h>
#include <kprintf.h>
#include <sched.h>
#include <sched_test.h>
#include <selftest.h>
#include <string.h>
#include <timer.h>

#define RUN_MS 200

struct task {
    const char *name;
    uint32_t ran[SCHED_SLOTS + 1];  /* ran[s] = times this task woke in slot s */
};

static struct task tasks[SCHED_SLOTS + 1];  /* [0] is main */
static int ntasks;
static volatile int recording;

static void note_slot(struct task *t)
{
    if (recording)
        t->ran[sched_slot()]++;
}

static void task_loop(void *arg)
{
    for (;;) {
        note_slot(arg);
        cpu_wait();             /* sleep until the next tick (or key) */
    }
}

static struct task *find_task(const char *name)
{
    for (int i = 0; i < ntasks; i++)
        if (strcmp(tasks[i].name, name) == 0)
            return &tasks[i];
    return NULL;
}

void sched_selftest(void)
{
    /* One record per distinct name: main, then each task in the config. */
    tasks[ntasks++].name = SCHED_SYSTEM_THREAD;
    for (int s = 1; s <= SCHED_SLOTS; s++) {
        const char *name = slot_config[s];
        if (name && !find_task(name)) {
            struct task *t = &tasks[ntasks++];
            t->name = name;
            check(thread_create(name, task_loop, t) != NULL, "sched: create task");
        }
    }

    sched_start();
    recording = 1;
    uint32_t t0 = timer_ms();
    while (timer_ms() - t0 < RUN_MS) {
        note_slot(&tasks[0]);   /* main: same as the tasks */
        cpu_wait();
    }
    recording = 0;

    kprintf("Slot scheduler: %d slots x 1 ms, ran %d ms:\n", SCHED_SLOTS, RUN_MS);
    int all_ok = 1;
    for (int i = 0; i < ntasks; i++) {
        struct task *t = &tasks[i];
        kprintf("  %s ran in slots:", t->name);
        for (int s = 1; s <= SCHED_SLOTS; s++) {
            const char *owner = sched_slot_owner(s);
            int mine = owner && strcmp(owner, t->name) == 0;
            if (t->ran[s])
                kprintf(" %d", s);
            if (mine != (t->ran[s] > 0))
                all_ok = 0;     /* ran where it shouldn't, or missed its slot */
        }
        kprintf("\n");
    }
    check(all_ok, "sched: every task ran in exactly its own slots");
}
