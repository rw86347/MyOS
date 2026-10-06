/*
 * pit.c — 8253/8254 Programmable Interval Timer: the 1 ms system tick
 *
 * The PIT has a crystal running at 1,193,182 Hz (a leftover from the
 * original IBM PC's TV-frequency crystal divided by 3). Channel 0 counts
 * down from a "divisor" we choose and fires IRQ 0 each time it hits zero:
 *
 *     interrupt rate = 1193182 / divisor
 *
 * For a 1 ms tick: 1193182 / 1000 = 1193 -> 1000.15 Hz (close enough;
 * the 0.015% error is about 13 seconds a day, fine until we have a clock).
 *
 * This tick will drive the time-slot scheduler: 20 slots x 1 ms (DESIGN.md).
 */
#include <cycles.h>
#include <hpet.h>
#include <interrupts.h>
#include <io.h>
#include <kprintf.h>
#include <sched.h>
#include <signal.h>
#include <serial.h>
#include <thread.h>
#include <timer.h>

#define PIT_FREQ     1193182
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

/* volatile: changed by the interrupt handler behind the main code's back,
   so the compiler must re-read it from memory every time. */
static volatile uint32_t ticks;
/* The same count with Linux's name and width (unsigned long: 32 bits on
   i386, 64 on x86_64), for drivers written to Linux's API (linux/jiffies.h). */
volatile unsigned long jiffies;

static void timer_irq(struct regs *r)
{
    ticks++;
    jiffies++;
    struct thread *t = thread_current();    /* (NULL until thread_init) */
    if (t && !t->is_idle) {             /* CPU time: whoever this tick interrupted */
        if ((r->cs & 3) == 3)
            t->utime++;
        else
            t->stime++;
    }
    cycles_tick(ticks);                 /* background calibration (cpu.c) */
    thread_timer_tick(ticks);           /* wake timed sleepers that are due */
    signal_alarm_tick(ticks);           /* SIGALRM for alarm/setitimer */
    ptimer_tick(ticks);                 /* timer_create's timers */
    serial_tick();                      /* COM1 output, if its IRQ is missing */
    sched_tick();                       /* next time slot; may switch threads */
}

/* Wait for the next tick, at most `limit` CPU cycles. 1 if it came. */
static int tick_within(uint64_t limit)
{
    uint32_t start = ticks;
    uint64_t t0 = cycles_now();
    while (ticks == start)
        if (cycles_now() - t0 > limit)
            return 0;
    return 1;
}

/* Does the PIT really tick? On the old PCs it does: the first tick, within
   1 ms, ends the wait. New Intel chipsets may switch its clock off (docs/
   hardware-msi-katana-gf66.md); then the HPET takes over IRQ 0 (hpet.c).
   50 million cycles is 10 ms on a 5 GHz CPU and 25 ms on a 2 GHz one,
   plenty for a 1 ms tick. Interrupts must be on. */
void timer_check(void)
{
    if (tick_within(50000000ull))
        return;
    if (hpet_legacy_start(TIMER_HZ) == 0 && tick_within(50000000ull))
        kprintf("timer: the PIT does not tick; the HPET gives the 1 ms tick\n");
    else
        kprintf("timer: NO TICK: the PIT does not count and there is no HPET."
                " Sleeps and time slots will not work.\n");
}

void timer_init(void)
{
    uint16_t divisor = PIT_FREQ / TIMER_HZ;

    /* Command 0x36: channel 0, write low byte then high byte, mode 3
       (square wave generator), binary counting. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, divisor >> 8);

    irq_install(0, timer_irq);
}

uint32_t timer_ms(void)
{
    return ticks * (1000 / TIMER_HZ);
}

void sleep_ms(uint32_t ms)
{
    uint32_t start = timer_ms();
    while (timer_ms() - start < ms)     /* subtraction handles wrap-around */
        cpu_wait();                     /* sleep until the next tick */
}
