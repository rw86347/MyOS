/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_DELAY_H
#define LINUX_DELAY_H

/* Waiting a while. udelay/ndelay/mdelay spin on the cycle counter (short
   waits for hardware, interrupts may be off). msleep and usleep_range give
   the CPU away: the tick is 1 ms, so a wait shorter than that spins
   instead of sleeping a whole tick (linux_lib.c). */
#include <linux/types.h>

void udelay(unsigned long us);
void ndelay(unsigned long ns);
#define mdelay(ms)   do { unsigned long _m = (ms); while (_m--) udelay(1000); } while (0)
void msleep(unsigned int ms);
unsigned long msleep_interruptible(unsigned int ms);
void usleep_range(unsigned long min, unsigned long max);
#define fsleep(us)   usleep_range(us, (us) * 2)
#define ssleep(s)    msleep((s) * 1000)

#endif
