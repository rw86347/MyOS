/*
 * timer_list.c — Linux's timer_list: run a function at a given jiffy.
 *
 * Pending timers are kept sorted by expiry, so adding one is a walk down
 * the (short) list and finding the next due one is free. One kernel
 * thread, "ktimers", sleeps exactly until the first expiry and runs every
 * timer that is due, with interrupts off — Linux's timer softirq. A timer
 * function may not sleep there, as on Linux. The 1 ms tick itself does no
 * timer work: it only wakes the thread when its sleep ends (thread.c).
 */
#include <interrupts.h>
#include <kprintf.h>
#include <linux/timer.h>
#include <thread.h>

static struct timer_list *pending;      /* sorted, earliest first */
static struct wait_queue wake;          /* ktimers sleeps here */

static void unlink(struct timer_list *t)
{
    for (struct timer_list **pp = &pending; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    t->next = NULL;
    t->pending = 0;
}

int mod_timer(struct timer_list *t, unsigned long expires)
{
    unsigned long f = irq_save();
    int was = t->pending;
    if (was)
        unlink(t);
    t->expires = expires;
    struct timer_list **pp = &pending;
    while (*pp && !time_after((*pp)->expires, expires))
        pp = &(*pp)->next;              /* after any with the same expiry: FIFO */
    t->next = *pp;
    *pp = t;
    t->pending = 1;
    if (pending == t)
        wq_wake_all(&wake);             /* a new first: ktimers re-arms */
    irq_restore(f);
    return was;
}

void add_timer(struct timer_list *t)
{
    mod_timer(t, t->expires);
}

int timer_delete(struct timer_list *t)
{
    unsigned long f = irq_save();
    int was = t->pending;
    if (was)
        unlink(t);
    irq_restore(f);
    return was;
}

static void ktimers(void *arg)
{
    (void)arg;
    irq_save();                         /* like a softirq: interrupts off */
    for (;;) {
        while (pending && !time_before(jiffies, pending->expires)) {
            struct timer_list *t = pending;
            pending = t->next;
            t->next = NULL;
            t->pending = 0;
            t->function(t);             /* may re-add itself */
        }
        uint32_t until = pending ? (uint32_t)pending->expires : 0;
        wq_sleep_until(&wake, pending && !until ? 1 : until);
    }
}

void timer_list_init(void)
{
    if (!thread_create("ktimers", ktimers, NULL))
        kprintf("timer_list: no thread\n");
}
