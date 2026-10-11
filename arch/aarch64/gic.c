/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * gic.c — GICv2 bring-up for QEMU virt (DTB: intc @ 0x08000000)
 */
#include "gic.h"

#include <stdint.h>

#define GICD_CTLR       0x000u
#define GICD_ISENABLER  0x100u
#define GICD_IPRIORITYR 0x400u
#define GICD_ITARGETSR  0x800u
#define GICD_ICFGR      0xc00u

#define GICC_CTLR  0x000u
#define GICC_PMR   0x004u
#define GICC_IAR   0x00cu
#define GICC_EOIR  0x010u

static inline void mmio_write32(uintptr_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

static inline uint32_t mmio_read32(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

void gic_init(void)
{
    unsigned int i;

    /* Distributor off, then configure. */
    mmio_write32(GICD_BASE, GICD_CTLR, 0);
    mmio_write32(GICC_BASE, GICC_CTLR, 0);

    /* Level-sensitive PPIs; enable timer IRQ with mid priority on CPU 0. */
    for (i = 0; i < 16; i++)
        mmio_write32(GICD_BASE, GICD_ICFGR + i * 4, 0);

    {
        uint32_t off = GICD_IPRIORITYR + (GIC_INT_TIMER_EL1 & ~3u);
        uint32_t pri = mmio_read32(GICD_BASE, off);

        pri &= ~(0xffu << ((GIC_INT_TIMER_EL1 % 4u) * 8u));
        pri |= 0xa0u << ((GIC_INT_TIMER_EL1 % 4u) * 8u);
        mmio_write32(GICD_BASE, off, pri);
    }
    {
        uint32_t off = GICD_ITARGETSR + (GIC_INT_TIMER_EL1 & ~3u);
        uint32_t tgt = mmio_read32(GICD_BASE, off);

        tgt &= ~(0xffu << ((GIC_INT_TIMER_EL1 % 4u) * 8u));
        tgt |= 1u << ((GIC_INT_TIMER_EL1 % 4u) * 8u);
        mmio_write32(GICD_BASE, off, tgt);
    }

    mmio_write32(GICD_BASE, GICD_ISENABLER + (GIC_INT_TIMER_EL1 / 32) * 4,
                 1u << (GIC_INT_TIMER_EL1 % 32));

    mmio_write32(GICD_BASE, GICD_CTLR, 1);
    mmio_write32(GICC_BASE, GICC_PMR, 0xff);
    mmio_write32(GICC_BASE, GICC_CTLR, 1);
}

void gic_enable_irq(unsigned int irq)
{
    mmio_write32(GICD_BASE, GICD_ISENABLER + (irq / 32) * 4, 1u << (irq % 32));
}

void gic_eoi(unsigned int irq)
{
    (void)irq;
    mmio_write32(GICC_BASE, GICC_EOIR, irq);
}

unsigned int gic_ack(void)
{
    return mmio_read32(GICC_BASE, GICC_IAR) & 0x3ffu;
}
