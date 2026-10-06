/*
 * workqueue.c — Linux's work queues: functions that run soon, in a thread,
 * where they may sleep. One kernel thread per workqueue runs its items in
 * the order they were queued; system_wq ("events") is the shared one.
 * A delayed_work is a timer (timer_list.c) that queues the work when it
 * fires. RCU's deferred frees (call_rcu, kfree_rcu) run on system_wq too:
 * by the time it gets to them, whoever queued them has left its read
 * section (rcupdate.h says why that is enough here).
 */
#include <interrupts.h>
#include <kprintf.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <stdarg.h>
#include <thread.h>

struct workqueue_struct {
    char name[16];
    struct work_struct *head, *tail;    /* waiting to run, in order */
    struct work_struct *running;
    struct wait_queue more;             /* the thread waits here for work */
    struct wait_queue done;             /* flushers wait here */
    int stop;
};

struct workqueue_struct *system_wq;

void timer_list_init(void);

void linux_init_work(struct work_struct *w, work_func_t fn)
{
    w->next = NULL;
    w->func = fn;
    w->wq = NULL;
    w->pending = 0;
}

static void worker(void *arg)
{
    struct workqueue_struct *wq = arg;
    irq_save();                         /* kernel threads run with interrupts off */
    for (;;) {
        while (wq->head) {
            struct work_struct *w = wq->head;
            wq->head = w->next;
            if (!wq->head)
                wq->tail = NULL;
            w->next = NULL;
            w->pending = 0;
            wq->running = w;            /* (w itself may be freed by func) */
            w->func(w);                 /* may sleep, may free w, may requeue it */
            wq->running = NULL;
            wq_wake_all(&wq->done);
        }
        if (wq->stop) {
            wq_wake_all(&wq->done);
            return;
        }
        wq_sleep(&wq->more);
    }
}

/* The work the current thread is running, or NULL. */
struct work_struct *linux_current_work(void)
{
    return NULL;                        /* (not tracked per thread yet) */
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
    (void)flags; (void)max_active;
    struct workqueue_struct *wq = kzalloc(sizeof *wq, GFP_KERNEL);
    if (!wq)
        return NULL;
    va_list ap;
    va_start(ap, max_active);
    vsnprintf(wq->name, sizeof wq->name, fmt, ap);
    va_end(ap);
    if (!thread_create(wq->name, worker, wq)) {
        kfree(wq);
        return NULL;
    }
    return wq;
}

void destroy_workqueue(struct workqueue_struct *wq)
{
    if (!wq)
        return;
    flush_workqueue(wq);
    unsigned long f = irq_save();
    wq->stop = 1;
    wq_wake_all(&wq->more);
    irq_restore(f);
    /* The thread may still be returning; its struct goes with it. */
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
    unsigned long f = irq_save();
    bool queued = !w->pending;
    if (queued) {                       /* already waiting: Linux doesn't queue twice */
        w->pending = 1;
        w->wq = wq;
        w->next = NULL;
        if (wq->tail)
            wq->tail->next = w;
        else
            wq->head = w;
        wq->tail = w;
        wq_wake_all(&wq->more);
    }
    irq_restore(f);
    return queued;
}

/* Take a waiting item off its queue. Interrupts off. */
static bool dequeue(struct work_struct *w)
{
    struct workqueue_struct *wq = w->wq;
    if (!w->pending || !wq)
        return false;
    struct work_struct *prev = NULL;
    for (struct work_struct *p = wq->head; p; prev = p, p = p->next)
        if (p == w) {
            if (prev)
                prev->next = w->next;
            else
                wq->head = w->next;
            if (wq->tail == w)
                wq->tail = prev;
            break;
        }
    w->next = NULL;
    w->pending = 0;
    return true;
}

bool cancel_work(struct work_struct *w)
{
    unsigned long f = irq_save();
    bool was = dequeue(w);
    irq_restore(f);
    return was;
}

/* Wait until w is neither waiting nor running. The queue's thread wakes
   `done` after every item. */
bool flush_work(struct work_struct *w)
{
    unsigned long f = irq_save();
    bool busy = w->wq && (w->pending || w->wq->running == w);
    while (w->wq && (w->pending || w->wq->running == w))
        wq_sleep(&w->wq->done);
    irq_restore(f);
    return busy;
}

bool cancel_work_sync(struct work_struct *w)
{
    unsigned long f = irq_save();
    bool was = dequeue(w);
    while (w->wq && w->wq->running == w)
        wq_sleep(&w->wq->done);
    irq_restore(f);
    return was;
}

void flush_workqueue(struct workqueue_struct *wq)
{
    unsigned long f = irq_save();
    while (wq->head || wq->running)
        wq_sleep(&wq->done);
    irq_restore(f);
}

void linux_delayed_work_timer(struct timer_list *t)
{
    struct delayed_work *dw = from_timer(dw, t, timer);
    queue_work(dw->wq ? dw->wq : system_wq, &dw->work);
}

bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
    unsigned long f = irq_save();
    bool queued = !dw->work.pending && !timer_pending(&dw->timer);
    if (queued) {
        dw->wq = wq;
        if (!delay)
            queue_work(wq, &dw->work);
        else
            mod_timer(&dw->timer, jiffies + delay);
    }
    irq_restore(f);
    return queued;
}

bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
    unsigned long f = irq_save();
    bool was = timer_delete(&dw->timer) | dequeue(&dw->work);
    dw->wq = wq;
    if (!delay)
        queue_work(wq, &dw->work);
    else
        mod_timer(&dw->timer, jiffies + delay);
    irq_restore(f);
    return was;
}

bool cancel_delayed_work(struct delayed_work *dw)
{
    unsigned long f = irq_save();
    bool was = timer_delete(&dw->timer) | dequeue(&dw->work);
    irq_restore(f);
    return was;
}

bool cancel_delayed_work_sync(struct delayed_work *dw)
{
    unsigned long f = irq_save();
    bool was = timer_delete(&dw->timer) | dequeue(&dw->work);
    while (dw->work.wq && dw->work.wq->running == &dw->work)
        wq_sleep(&dw->work.wq->done);
    irq_restore(f);
    return was;
}

bool flush_delayed_work(struct delayed_work *dw)
{
    unsigned long f = irq_save();
    if (timer_delete(&dw->timer))
        queue_work(dw->wq ? dw->wq : system_wq, &dw->work);
    irq_restore(f);
    return flush_work(&dw->work);
}

/* --- RCU's deferred callbacks ------------------------------------------------ */

static struct rcu_head *rcu_list;
static struct work_struct rcu_work;

static void rcu_run(struct work_struct *w)
{
    (void)w;
    unsigned long f = irq_save();
    struct rcu_head *h = rcu_list;
    rcu_list = NULL;
    irq_restore(f);
    while (h) {
        struct rcu_head *next = h->next;
        h->func(h);
        h = next;
    }
}

void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *))
{
    unsigned long f = irq_save();
    head->func = func;
    head->next = rcu_list;
    rcu_list = head;
    queue_work(system_wq, &rcu_work);
    irq_restore(f);
}

/* kfree_rcu: the object is freed later. A small record says what to free
   (Linux reuses an rcu_head inside the object; this way any object can be
   passed). If memory is short, we fall back to waiting for the queue. */
struct rcu_free { struct rcu_head head; void *ptr; };

static void rcu_free_one(struct rcu_head *h)
{
    struct rcu_free *r = container_of(h, struct rcu_free, head);
    kfree(r->ptr);
    kfree(r);
}

void kfree_rcu_ptr(void *ptr)
{
    if (!ptr)
        return;
    struct rcu_free *r = kmalloc(sizeof *r, GFP_ATOMIC);
    if (!r) {
        rcu_flush();
        kfree(ptr);
        return;
    }
    r->ptr = ptr;
    call_rcu(&r->head, rcu_free_one);
}

void rcu_flush(void)
{
    flush_work(&rcu_work);
}

/* --- start ------------------------------------------------------------------ */

void shim_init(void)
{
    timer_list_init();
    INIT_WORK(&rcu_work, rcu_run);
    system_wq = alloc_workqueue("events", 0, 0);
    if (!system_wq)
        kprintf("workqueue: no system_wq thread\n");
}
