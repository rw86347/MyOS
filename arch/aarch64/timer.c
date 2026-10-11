/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * timer.c — ARM architected timer @ EL1 (CNTP_*), 1 ms tick via GIC PPI 30
 */
#include "gic.h"
#include "arm64_timer.h"

#include <stdint.h>

static volatile uint32_t s_ticks;
static uint32_t s_hz;

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint32_t tval)
{
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"((uint64_t)tval));
}

static inline void write_cntp_ctl(uint32_t ctl)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"((uint64_t)ctl));
}

static inline uint32_t read_cntp_ctl(void)
{
    uint64_t v;

    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(v));
    return (uint32_t)v;
}

void timer_init(void)
{
    s_hz = (uint32_t)read_cntfrq();
    if (s_hz == 0)
        s_hz = 62500000u; /* QEMU virt default when CNTFRQ not set */

    write_cntp_ctl(0);
    write_cntp_tval(s_hz / TIMER_HZ);
    write_cntp_ctl(1); /* enable, no mask */
    gic_init();
}

void timer_on_irq(void)
{
    if (read_cntp_ctl() & (1u << 2)) {
        s_ticks++;
        write_cntp_tval(s_hz / TIMER_HZ);
    }
}

uint32_t timer_ms(void)
{
    return s_ticks;
}

uint32_t timer_ticks(void)
{
    return s_ticks;
}
