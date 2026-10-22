/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * suspend.c — sleep: suspend to idle (Linux's "s2idle", Intel's S0ix)
 *
 * A Mac sleeps when its lid shuts and wakes, locked, when it opens. New
 * laptops (the GPD Pocket 4, the MSI Katana) do it the same way from the
 * OS's side: no S3 ("suspend to RAM") any more, only S0 with the CPU and
 * the chipset idling as deeply as they can ("modern standby"). Older ones
 * that still offer S3 can do this too (the boot log says which they
 * have: acpi_button.c). The OS's part is to make the machine quiet
 * enough for that:
 *
 *   1. stop every program at its next return to user mode (the freezer):
 *      programs that wait keep waiting, ones that run stop within a slot
 *      or two. If one has not stopped after 200 ms: no sleep (-EBUSY).
 *   2. write the disks' changed blocks (a battery may run out asleep)
 *   3. the screen and its backlight off
 *   4. every 8259 interrupt off but those that wake (the keyboard, the
 *      mouse, ACPI's for the lid and the power button, a USB keyboard's)
 *      and the serial port's, for the log. Linux drivers' interrupts are
 *      held, and run on waking.
 *   5. the idle thread stops the 1 ms tick (thread.c): the CPU sleeps
 *      until one of those interrupts. No timer wakes it: a kernel thread's
 *      timed wait that ends within 100 ms is let finish first; programs'
 *      timers wait, since their clock (CLOCK_MONOTONIC) stands still
 *      asleep, as on Linux.
 *
 * Waking undoes it in reverse. The wall clock (CLOCK_REALTIME) and
 * CLOCK_BOOTTIME jump by the time the tick was off.
 *
 * What wakes it: the lid opening, the power button (reported as KEY_WAKEUP,
 * as Linux does, so it isn't taken for "sleep" again), a key or a click
 * while the lid is open. The key isn't typed.
 *
 * A program decides when (myos-powerd on the live system): it reads the
 * lid and the power button (evdev.c) and writes "mem" to /sys/power/state;
 * the write returns once the machine is awake again.
 */
#include <acpi.h>
#include <bcache.h>
#include <cycles.h>
#include <errno.h>
#include <interrupts.h>
#include <kprintf.h>
#include <pcache.h>
#include <pic.h>
#include <suspend.h>
#include <thread.h>
#include <timer.h>

#define FREEZE_MS 200
/* Lines that stay on asleep: the keyboard (1), the second 8259 (2), the
   serial ports (3, 4) and the PS/2 mouse (12). The tick (0) is the idle
   thread's; drivers add theirs (suspend_keep_irq). */
#define KEEP_IRQS ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 12))

volatile int suspend_asleep;
static volatile int freezing;           /* programs stop at their user-mode edge */
static volatile int wake_reason;
static volatile int sleeping;           /* suspend_enter is under way */
static uint32_t woke_ms;
static uint16_t keep_irqs = KEEP_IRQS;
static struct wait_queue thaw_wq, wake_wq;

static const char *const reasons[] = {
    "nothing", "the lid", "the power button", "a key", "the self-test",
};

void suspend_keep_irq(int irq)
{
    if (irq >= 0 && irq < 16)
        keep_irqs |= (uint16_t)(1u << irq);
}

/* --- the freezer ---------------------------------------------------------- */

void suspend_freeze_point(void)
{
    if (!freezing)
        return;
    uint32_t f = irq_save();
    while (freezing)
        wq_sleep(&thaw_wq);
    irq_restore(f);
}

/* Every program's thread to its freeze point. Threads that wait in the
   kernel count as stopped: they stop at the edge when they wake. */
static int freeze(void)
{
    struct thread *me = thread_current();
    uint32_t end = timer_ms() + FREEZE_MS;
    uint32_t f = irq_save();
    freezing = 1;
    for (;;) {
        int running = 0;
        for (int i = 0; i < THREAD_MAX; i++) {
            struct thread *t = thread_at(i);
            if (!t->proc || t == me || t->state == THREAD_UNUSED || t->state == THREAD_DEAD)
                continue;
            t->sig_check = 1;           /* (again: one made meanwhile, too) */
            running += t->state == THREAD_READY;
        }
        if (!running)
            break;
        if ((int32_t)(timer_ms() - end) >= 0) {
            irq_restore(f);
            klog("sleep: %d program threads did not stop in %d ms\n", running, FREEZE_MS);
            return -EBUSY;
        }
        wq_sleep_until(&(struct wait_queue){ 0 }, timer_ms() + 1);
    }
    irq_restore(f);
    return 0;
}

static void thaw(void)
{
    uint32_t f = irq_save();
    freezing = 0;
    wq_wake_all(&thaw_wq);
    irq_restore(f);
}

/* --- waking ---------------------------------------------------------------- */

/* 1 if the machine was going to sleep or asleep: this wakes it, and the
   caller shouldn't pass the event on as a request to sleep. */
int suspend_wake(int reason)
{
    uint32_t f = irq_save();
    int was = sleeping;
    if (sleeping && wake_reason == WAKE_NONE) {
        wake_reason = reason;
        woke_ms = timer_ms();
        wq_wake_all(&wake_wq);
    }
    irq_restore(f);
    return was;
}

void suspend_input(void)
{
    if (suspend_asleep && !acpi_lid_closed())
        suspend_wake(WAKE_KEY);
}

/* --- going to sleep and back --------------------------------------------- */

int suspend_enter(void)
{
    uint32_t f = irq_save();
    if (sleeping) {
        irq_restore(f);
        return -EBUSY;
    }
    sleeping = 1;
    wake_reason = WAKE_NONE;
    irq_restore(f);
    uint32_t t0 = timer_ms();
    klog("sleep: going to sleep (s2idle)\n");
    int err = freeze();
    if (err) {
        thaw();
        sleeping = 0;
        return err;
    }
    pcache_sync();                      /* file data, then the blocks under it */
    bsync();
    drm_suspend(1);
    int level = backlight_get();
    if (level > 0)
        backlight_set(0);

    f = irq_save();
    uint16_t forced = (uint16_t)(~pic_masks() & ~keep_irqs & ~1u);
    pic_set_masks(pic_masks() | forced);
    linux_irq_suspend(1);
    uint32_t n0, n1;
    uint64_t c0 = timer_paused(&n0), s0 = cycles_now();
    suspend_asleep = 1;
    klog("sleep: asleep (%u ms to stop)\n", (unsigned)(timer_ms() - t0));
    while (wake_reason == WAKE_NONE)
        wq_sleep(&wake_wq);
    suspend_asleep = 0;
    uint64_t c1 = timer_paused(&n1), s1 = cycles_now();
    linux_irq_suspend(0);
    pic_set_masks(pic_masks() & ~forced);
    irq_restore(f);

    uint64_t off = (c1 - c0) / cycles_per_us(), all = (s1 - s0) / cycles_per_us();
    time_add_sleep_ns((int64_t)off * 1000);     /* ticks counted the rest */
    if (level > 0)
        backlight_set(level);
    drm_suspend(0);
    thaw();
    klog("sleep: woken by %s after %u.%u s asleep (the tick off %u%% of it, the CPU woke"
         " %u times), awake in %u ms\n", reasons[wake_reason], (unsigned)(all / 1000000),
         (unsigned)(all / 100000 % 10), (unsigned)(all ? off * 100 / all : 0),
         (unsigned)(n1 - n0), (unsigned)(timer_ms() - woke_ms));
    sleeping = 0;
    return 0;
}
