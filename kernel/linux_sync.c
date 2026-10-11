/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * linux_sync.c — Linux's mutexes and completions on MyOS's wait queues.
 * Interrupts are off around each check-then-sleep, so a wake-up (from
 * another thread or an interrupt handler) can't slip in between.
 */
#include <interrupts.h>
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <signal.h>

void mutex_lock(struct mutex *m)
{
    unsigned long f = irq_save();
    while (m->owner)
        wq_sleep(&m->wait);
    m->owner = thread_current();
    irq_restore(f);
}

int mutex_lock_interruptible(struct mutex *m)
{
    mutex_lock(m);
    return 0;
}

int mutex_trylock(struct mutex *m)
{
    unsigned long f = irq_save();
    int got = !m->owner;
    if (got)
        m->owner = thread_current();
    irq_restore(f);
    return got;
}

void mutex_unlock(struct mutex *m)
{
    unsigned long f = irq_save();
    m->owner = NULL;
    wq_wake_all(&m->wait);              /* each waiter re-checks; one wins */
    irq_restore(f);
}

#define DONE_ALL (~0u >> 1)

void complete(struct completion *c)
{
    unsigned long f = irq_save();
    if (c->done != DONE_ALL)
        c->done++;
    wq_wake_all(&c->wait);
    irq_restore(f);
}

void complete_all(struct completion *c)
{
    unsigned long f = irq_save();
    c->done = DONE_ALL;
    wq_wake_all(&c->wait);
    irq_restore(f);
}

static void take(struct completion *c)
{
    if (c->done != DONE_ALL)
        c->done--;
}

void wait_for_completion(struct completion *c)
{
    unsigned long f = irq_save();
    while (!c->done)
        wq_sleep(&c->wait);
    take(c);
    irq_restore(f);
}

unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout)
{
    unsigned long f = irq_save();
    unsigned long end = jiffies + timeout;
    while (!c->done && time_before(jiffies, end))
        wq_sleep_until(&c->wait, (uint32_t)end ? (uint32_t)end : 1);
    unsigned long left = 0;
    if (c->done) {
        take(c);
        left = time_before(jiffies, end) ? end - jiffies : 1;
    }
    irq_restore(f);
    return left;
}

int wait_for_completion_interruptible(struct completion *c)
{
    unsigned long f = irq_save();
    int err = 0;
    while (!c->done) {
        if (signal_pending()) {
            err = -512;                 /* -ERESTARTSYS */
            break;
        }
        wq_sleep(&c->wait);
    }
    if (!err)
        take(c);
    irq_restore(f);
    return err;
}

long wait_for_completion_interruptible_timeout(struct completion *c, unsigned long timeout)
{
    return (long)wait_for_completion_timeout(c, timeout);
}

bool try_wait_for_completion(struct completion *c)
{
    unsigned long f = irq_save();
    bool got = c->done != 0;
    if (got)
        take(c);
    irq_restore(f);
    return got;
}

bool completion_done(struct completion *c)
{
    return c->done != 0;
}
