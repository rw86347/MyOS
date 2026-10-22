/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef SUSPEND_H
#define SUSPEND_H

#include <stdint.h>

/*
 * Sleep: suspend to idle, Linux's "s2idle" (kernel/suspend.c). Programs
 * stop, the screen goes dark, the CPU sleeps with the tick off until the
 * lid opens, the power button is pressed or a key is typed.
 *
 *   echo mem > /sys/power/state     sleep now; the write returns on waking
 */
enum wake_reason { WAKE_NONE, WAKE_LID, WAKE_POWER, WAKE_KEY, WAKE_TEST };

int  suspend_enter(void);               /* 0 after waking; -EBUSY: a program would not stop */
/* Any context, interrupts too. 1 if the machine was going to sleep or
   asleep: the event woke it and is not a request to sleep. */
int  suspend_wake(int reason);
void suspend_input(void);               /* evdev: a key or a click while asleep */
void suspend_freeze_point(void);        /* signal_check: programs wait here while asleep */
void suspend_keep_irq(int irq);         /* a driver's 8259 line that may wake us (a keyboard's) */

/* 1 from the moment programs are stopped until the wake-up starts: the idle
   thread may stop the tick, and key presses wake instead of being typed. */
extern volatile int suspend_asleep;

/* The parts in drivers, called on the way down (1) and up (0). */
void drm_suspend(int sleep);            /* drm_core.c: the main screen off, and on again */
void linux_irq_suspend(int sleep);      /* linux_irq.c: Linux drivers' interrupts held */
void timer_pause(void);                 /* pit.c: the tick off (interrupts off) ... */
void timer_resume(void);                /* ... and on, counting the time it was off */
uint64_t timer_paused(uint32_t *times); /* cycles it was off since boot; how many times */
void time_add_sleep_ns(int64_t ns);     /* sys_info.c: the wall clock and BOOTTIME jump */

#endif
