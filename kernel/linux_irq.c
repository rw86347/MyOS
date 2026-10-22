/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * linux_irq.c — Linux's request_irq / request_threaded_irq on MyOS's IRQs.
 *
 * One native handler serves every IRQ a Linux driver asked for: it finds
 * the IRQ from the vector and runs that IRQ's Linux handlers in order
 * (lines are shared). A handler that answers IRQ_WAKE_THREAD gets its
 * thread part run by the IRQ's own kernel thread ("irq/16-iwlwifi"), where
 * it may sleep. disable_irq masks an 8259 line; an MSI that arrives while
 * its IRQ is disabled is remembered and handled when it is enabled again.
 */
#include <interrupts.h>
#include <kprintf.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <pic.h>
#include <suspend.h>
#include <thread.h>

struct lirq {
    struct lirq *next;
    irq_handler_t handler, thread_fn;
    void *dev_id;
    const char *name;
    volatile int thread_pending;
};

static struct lirq *handlers[NR_IRQS];
static struct thread *threads[NR_IRQS];
static struct wait_queue irq_wake[NR_IRQS];
static struct wait_queue thread_done[NR_IRQS];
static volatile uint8_t disabled[NR_IRQS], missed[NR_IRQS], thread_busy[NR_IRQS];
static volatile uint8_t asleep;         /* sleep (suspend.c): every handler held */

/* Interrupts are off here (an interrupt), as everywhere in this file. */
static void run_handlers(int irq)
{
    int wake = 0;
    for (struct lirq *l = handlers[irq]; l; l = l->next) {
        irqreturn_t r = l->handler ? l->handler(irq, l->dev_id) : IRQ_WAKE_THREAD;
        if (r == IRQ_WAKE_THREAD && l->thread_fn) {
            l->thread_pending = 1;
            wake = 1;
        }
    }
    if (wake)
        wq_wake_all(&irq_wake[irq]);
}

static void entry(struct regs *r)
{
    int irq = (int)r->vector - PIC_IRQ_BASE;
    if (irq < 0 || irq >= NR_IRQS)
        return;
    if (disabled[irq] || (asleep && irq >= IRQ_MSI_FIRST)) {
        missed[irq] = 1;                /* an MSI: replayed by enable_irq or on waking */
        return;
    }
    run_handlers(irq);
}

/* Asleep, a Linux driver's device may still interrupt (Wi-Fi hears a
   beacon); its handlers wait for the wake-up, as Linux's
   suspend_device_irqs makes them. That is for MSIs: suspend.c masks the
   8259 lines, and one it leaves on is shared with a wake-up device, where
   a level-triggered line not answered would come back at once. Interrupts
   are off. */
void linux_irq_suspend(int sleep)
{
    asleep = (uint8_t)sleep;
    if (sleep)
        return;
    for (int irq = 0; irq < NR_IRQS; irq++)
        if (missed[irq] && !disabled[irq]) {
            missed[irq] = 0;
            run_handlers(irq);
        }
}

static void irq_thread(void *arg)
{
    int irq = (int)(intptr_t)arg;
    irq_save();
    for (;;) {
        int ran = 0;
        thread_busy[irq] = 1;
        for (struct lirq *l = handlers[irq]; l; l = l->next)
            if (l->thread_pending) {
                l->thread_pending = 0;
                l->thread_fn(irq, l->dev_id);
                ran = 1;
            }
        thread_busy[irq] = 0;
        wq_wake_all(&thread_done[irq]);
        if (!ran)
            wq_sleep(&irq_wake[irq]);
    }
}

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                         unsigned long flags, const char *name, void *dev_id)
{
    (void)flags;
    if (irq >= NR_IRQS || (!handler && !thread_fn))
        return -EINVAL;
    struct lirq *l = kzalloc(sizeof *l, GFP_KERNEL);
    if (!l)
        return -ENOMEM;
    l->handler = handler;
    l->thread_fn = thread_fn;
    l->dev_id = dev_id;
    l->name = name;
    if (thread_fn && !threads[irq]) {
        char tname[16];
        snprintf(tname, sizeof tname, "irq/%u-%s", irq, name ? name : "");
        threads[irq] = thread_create(tname, irq_thread, (void *)(intptr_t)irq);
        if (!threads[irq]) {
            kfree(l);
            return -ENOMEM;
        }
    }
    unsigned long f = irq_save();
    int first = !handlers[irq];
    struct lirq **pp = &handlers[irq];
    while (*pp)
        pp = &(*pp)->next;
    *pp = l;
    irq_restore(f);
    if (first)
        irq_install((int)irq, entry);   /* (unmasks an 8259 line) */
    return 0;
}

const void *free_irq(unsigned int irq, void *dev_id)
{
    if (irq >= NR_IRQS)
        return NULL;
    unsigned long f = irq_save();
    const char *name = NULL;
    for (struct lirq **pp = &handlers[irq]; *pp; pp = &(*pp)->next)
        if ((*pp)->dev_id == dev_id) {
            struct lirq *l = *pp;
            *pp = l->next;
            while (thread_busy[irq] || l->thread_pending)
                wq_sleep(&thread_done[irq]);    /* its thread part may be running */
            name = l->name;
            kfree(l);
            break;
        }
    if (!handlers[irq] && irq < IRQ_MSI_FIRST)
        pic_mask((int)irq);
    irq_restore(f);
    return name;
}

struct devm_irq { unsigned int irq; void *dev_id; };

static void devm_irq_free(void *p)
{
    struct devm_irq *d = p;
    free_irq(d->irq, d->dev_id);
    kfree(d);
}

int devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
                              irq_handler_t thread_fn, unsigned long flags, const char *name,
                              void *dev_id)
{
    struct devm_irq *d = kmalloc(sizeof *d, GFP_KERNEL);
    if (!d)
        return -ENOMEM;
    int err = request_threaded_irq(irq, handler, thread_fn, flags, name, dev_id);
    if (err) {
        kfree(d);
        return err;
    }
    d->irq = irq;
    d->dev_id = dev_id;
    return devm_add_action_or_reset(dev, devm_irq_free, d);
}

void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
    (void)dev;
    free_irq(irq, dev_id);              /* (its devres record stays, harmless) */
}

void disable_irq_nosync(unsigned int irq)
{
    if (irq >= NR_IRQS)
        return;
    disabled[irq] = 1;
    if (irq < IRQ_MSI_FIRST)
        pic_mask((int)irq);
}

void synchronize_irq(unsigned int irq)
{
    if (irq >= NR_IRQS)
        return;
    unsigned long f = irq_save();
    for (;;) {
        int pending = thread_busy[irq];
        for (struct lirq *l = handlers[irq]; l; l = l->next)
            pending |= l->thread_pending;
        if (!pending)
            break;
        wq_sleep(&thread_done[irq]);
    }
    irq_restore(f);
}

void disable_irq(unsigned int irq)
{
    disable_irq_nosync(irq);
    synchronize_irq(irq);
}

void enable_irq(unsigned int irq)
{
    if (irq >= NR_IRQS)
        return;
    unsigned long f = irq_save();
    disabled[irq] = 0;
    if (irq < IRQ_MSI_FIRST)
        pic_unmask((int)irq);
    if (missed[irq]) {
        missed[irq] = 0;
        run_handlers((int)irq);
    }
    irq_restore(f);
}
