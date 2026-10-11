/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * hpet.c — the High Precision Event Timer, standing in for a silent PIT
 *
 * New Intel chipsets may switch the 8254 PIT's clock off to save power
 * ("8254 clock gating"; the MSI Katana, Tiger Lake). Then IRQ 0 never
 * comes: no tick, no time slot ends, no sleep ever wakes up. Every PC
 * chipset since about 2005 also has an HPET: a free-running counter
 * (Intel's counts at 24 MHz) and a few comparators, as memory-mapped
 * registers. Its "legacy replacement" mode was made for exactly this:
 * timer 0 then raises IRQ 0 on the 8259, as the PIT did, so the rest of
 * the kernel (pit.c's tick handler) doesn't notice the difference.
 *
 * Where it is comes from ACPI's HPET table, which we don't read yet.
 * Intel and AMD chipsets put it at 0xFED00000 and firmware leaves it
 * there; with nothing at that address, reads give all ones.
 */
#include <hpet.h>
#include <linux/io.h>

#define HPET_BASE   0xFED00000u
#define GCAP_ID     0x000       /* low: capabilities; high: the count period, in fs */
#define GEN_CONF    0x010
#define MAIN_CNT    0x0F0
#define T0_CONF     0x100
#define T0_COMP     0x108

#define LEG_RT_CAP  (1u << 15)  /* GCAP_ID: can take over IRQ 0 */
#define ENABLE_CNF  (1u << 0)   /* GEN_CONF: the counter runs */
#define LEG_RT_CNF  (1u << 1)   /* GEN_CONF: timer 0 -> IRQ 0, timer 1 -> IRQ 8 */
#define TN_LEVEL    (1u << 1)   /* timer config: level-triggered (we want edge) */
#define TN_INT_ENB  (1u << 2)   /* raise its interrupt */
#define TN_PERIODIC (1u << 3)
#define TN_PER_CAP  (1u << 4)   /* (read-only) can do periodic */
#define TN_SETVAL   (1u << 6)   /* the next comparator write also sets the period */
#define TN_32BIT    (1u << 8)
#define TN_FSB      (1u << 14)  /* deliver as an MSI (not wanted here) */

int hpet_legacy_start(uint32_t hz)
{
    volatile uint8_t *h = ioremap(HPET_BASE, 0x400);
    if (!h)
        return -1;
    uint32_t cap = readl(h + GCAP_ID);
    uint32_t period_fs = readl(h + GCAP_ID + 4);
    /* Nothing there (all ones), or not what the spec allows: a period of
       at most 100 ns, legacy routing, a timer 0 that can repeat. */
    if (cap == 0xFFFFFFFFu || period_fs == 0 || period_fs > 100000000u ||
        !(cap & LEG_RT_CAP) || !(readl(h + T0_CONF) & TN_PER_CAP))
        return -1;
    uint32_t count = (uint32_t)(1000000000000000ull / period_fs / hz);

    writel(0, h + GEN_CONF);                    /* stop the counter, then set it up */
    writel(0, h + MAIN_CNT);
    writel(0, h + MAIN_CNT + 4);
    uint32_t conf = readl(h + T0_CONF) & ~(TN_LEVEL | TN_FSB);
    writel(conf | TN_INT_ENB | TN_PERIODIC | TN_SETVAL | TN_32BIT, h + T0_CONF);
    writel(count, h + T0_COMP);                 /* first interrupt at `count` */
    writel(count, h + T0_COMP);                 /* and every `count` after (some
                                                   chips take the period only from
                                                   this second write, as Linux does) */
    writel(ENABLE_CNF | LEG_RT_CNF, h + GEN_CONF);
    return 0;
}
