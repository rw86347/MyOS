/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_TIMER_H
#define LINUX_TIMER_H

/*
 * timer_list: run a function at a given jiffy. Due timers run in one
 * kernel thread ("ktimers", kernel/timer_list.c) with interrupts off, as
 * Linux runs them in its timer softirq: they may not sleep. The pending
 * ones are kept sorted, so the thread sleeps exactly until the first is
 * due and the 1 ms tick itself does no timer work at all.
 */
#include <linux/ktime.h>
#include <linux/compiler.h>
#include <linux/jiffies.h>
#include <linux/list.h>

struct timer_list {
    struct timer_list *next;        /* the sorted list of pending timers */
    unsigned long expires;
    void (*function)(struct timer_list *);
    unsigned int flags;
    int pending;
};

#define TIMER_DEFERRABLE 0x1
#define TIMER_IRQSAFE    0x2

#define from_timer(var, t, field)   container_of(t, __typeof__(*var), field)
#define timer_container_of(var, t, field) from_timer(var, t, field)
static inline void timer_setup(struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags)
{
    t->next = NULL;
    t->expires = 0;
    t->function = fn;
    t->flags = flags;
    t->pending = 0;
}
#define DEFINE_TIMER(name, fn) struct timer_list name = { NULL, 0, fn, 0, 0 }
void add_timer(struct timer_list *t);
int  mod_timer(struct timer_list *t, unsigned long expires);    /* 1 if it was pending */
int  timer_delete(struct timer_list *t);                        /* 1 if it was pending */
static inline int timer_pending(const struct timer_list *t) { return t->pending; }
/* A due timer's function runs to the end without sleeping, and nothing
   else runs meanwhile; so once this returns it isn't running either. */
#define timer_delete_sync(t)        timer_delete(t)
#define del_timer(t)                timer_delete(t)
#define del_timer_sync(t)           timer_delete(t)
#define timer_shutdown_sync(t)      timer_delete(t)
#define timer_shutdown(t)           timer_delete(t)
#define mod_timer_pending(t, e)     ((t)->pending ? mod_timer(t, e) : 0)

#endif
