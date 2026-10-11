/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_WORKQUEUE_H
#define LINUX_WORKQUEUE_H

/*
 * Work queues: "run this function soon, in a thread" — where it may sleep
 * (lock a mutex, wait for the hardware), which an interrupt handler or a
 * timer may not. Each workqueue is one kernel thread running its items in
 * order (kernel/workqueue.c); system_wq and friends are one shared thread.
 * A delayed_work is a timer that queues the work when it fires.
 */
#include <linux/timer.h>
#include <linux/types.h>

struct workqueue_struct;
struct work_struct {
    struct work_struct *next;
    void (*func)(struct work_struct *);
    struct workqueue_struct *wq;    /* where it was last queued */
    u8 pending;
};
struct delayed_work {
    struct work_struct work;
    struct timer_list timer;
    struct workqueue_struct *wq;    /* where the timer will queue it */
};
typedef void (*work_func_t)(struct work_struct *);

void linux_init_work(struct work_struct *w, work_func_t fn);
void linux_delayed_work_timer(struct timer_list *t);
#define INIT_WORK(w, fn)          linux_init_work((w), (fn))
#define INIT_DELAYED_WORK(dw, fn) do { linux_init_work(&(dw)->work, (fn)); \
        timer_setup(&(dw)->timer, linux_delayed_work_timer, 0); (dw)->wq = NULL; } while (0)
#define INIT_DEFERRABLE_WORK(dw, fn) INIT_DELAYED_WORK(dw, fn)
#define DECLARE_WORK(n, fn)       struct work_struct n = { NULL, fn, NULL, 0 }
#define to_delayed_work(w)        container_of(w, struct delayed_work, work)
#define work_pending(w)           ((w)->pending)
#define delayed_work_pending(dw)  ((dw)->work.pending || timer_pending(&(dw)->timer))

#define WQ_UNBOUND       (1 << 1)
#define WQ_FREEZABLE     (1 << 2)
#define WQ_MEM_RECLAIM   (1 << 3)
#define WQ_HIGHPRI       (1 << 4)
#define WQ_CPU_INTENSIVE (1 << 5)
#define WQ_SYSFS         (1 << 6)
#define WQ_BH            (1 << 0)
#define __WQ_ORDERED     (1 << 17)

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
#define alloc_ordered_workqueue(fmt, flags, ...) \
    alloc_workqueue(fmt, (flags) | __WQ_ORDERED, 1, ##__VA_ARGS__)
#define create_singlethread_workqueue(name) alloc_ordered_workqueue("%s", 0, name)
void destroy_workqueue(struct workqueue_struct *wq);
extern struct workqueue_struct *system_wq;
#define system_unbound_wq    system_wq
#define system_highpri_wq    system_wq
#define system_long_wq       system_wq
#define system_freezable_wq  system_wq
#define system_power_efficient_wq system_wq
#define system_percpu_wq     system_wq
#define system_dfl_wq        system_wq

bool queue_work(struct workqueue_struct *wq, struct work_struct *w);
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay);
#define schedule_work(w)                  queue_work(system_wq, w)
#define schedule_delayed_work(dw, d)      queue_delayed_work(system_wq, dw, d)
#define queue_work_on(cpu, wq, w)         queue_work(wq, w)
#define queue_delayed_work_on(cpu, wq, dw, d) queue_delayed_work(wq, dw, d)
bool cancel_work(struct work_struct *w);
bool cancel_work_sync(struct work_struct *w);
bool cancel_delayed_work(struct delayed_work *dw);
bool cancel_delayed_work_sync(struct delayed_work *dw);
bool flush_work(struct work_struct *w);
bool flush_delayed_work(struct delayed_work *dw);
void flush_workqueue(struct workqueue_struct *wq);
#define drain_workqueue(wq)  flush_workqueue(wq)
#define current_work()       linux_current_work()
struct work_struct *linux_current_work(void);

#endif
