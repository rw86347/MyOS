/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_GIC_H
#define ARCH_AARCH64_GIC_H

/* QEMU virt DTB: arm,cortex-a15-gic (GICv2). */
#define GICD_BASE  0x08000000u
#define GICC_BASE  0x08010000u

#define GIC_INT_TIMER_EL1  30u /* PPI 14: CNTPNSIRQ (non-secure physical timer) */

void gic_init(void);
void gic_enable_irq(unsigned int irq);
unsigned int gic_ack(void);
void gic_eoi(unsigned int irq);

#endif
