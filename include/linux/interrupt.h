/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_INTERRUPT_H
#define LINUX_INTERRUPT_H

/*
 * Interrupt handlers, Linux's way (kernel/linux_irq.c). A handler runs in
 * the interrupt and answers IRQ_HANDLED (it was ours), IRQ_NONE (not ours:
 * lines are shared) or IRQ_WAKE_THREAD: then the handler's thread part
 * runs soon in a kernel thread of its own, where it may take mutexes and
 * sleep. The IRQ numbers are MyOS's: 0-15 the 8259's lines, 16-31 MSI.
 */
#include <linux/compiler.h>
#include <linux/types.h>

typedef enum irqreturn { IRQ_NONE = 0, IRQ_HANDLED = 1, IRQ_WAKE_THREAD = 2 } irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);
#define IRQ_RETVAL(x)  ((x) ? IRQ_HANDLED : IRQ_NONE)

#define IRQF_SHARED     0x00000080
#define IRQF_ONESHOT    0x00002000
#define IRQF_NO_SUSPEND 0x00004000
#define IRQF_NO_THREAD  0x00010000
#define IRQF_TRIGGER_NONE 0

struct device;
int  request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                          unsigned long flags, const char *name, void *dev_id);
static inline int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                              const char *name, void *dev_id)
{ return request_threaded_irq(irq, handler, NULL, flags, name, dev_id); }
const void *free_irq(unsigned int irq, void *dev_id);
int  devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
                               irq_handler_t thread_fn, unsigned long flags, const char *name,
                               void *dev_id);
#define devm_request_irq(dev, irq, h, f, n, id) devm_request_threaded_irq(dev, irq, h, NULL, f, n, id)
void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);
void enable_irq(unsigned int irq);
void synchronize_irq(unsigned int irq);
static inline int irq_set_affinity_hint(unsigned int irq, const void *m) { (void)irq; (void)m; return 0; }
#define irq_update_affinity_hint(irq, m) irq_set_affinity_hint(irq, m)

#endif
