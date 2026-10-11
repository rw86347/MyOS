/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define TIMER_HZ 1000           /* ticks per second: one tick = 1 ms */

void     timer_init(void);      /* start the tick (needs interrupts on to count) */
void     timer_check(void);     /* interrupts on: no PIT tick? use the HPET */
uint32_t timer_ms(void);        /* milliseconds since timer_init (wraps after 49 days) */
void     sleep_ms(uint32_t ms); /* busy-wait using the tick; interrupts must be on */

#endif
