/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_ARM64_TIMER_H
#define ARCH_AARCH64_ARM64_TIMER_H

#include <stdint.h>

#define TIMER_HZ 1000

void     timer_init(void);
void     timer_on_irq(void);
uint32_t timer_ms(void);
uint32_t timer_ticks(void);

#endif
