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
#include <interrupts.h>
#include <io.h>
#include <sched.h>
#include <signal.h>
#include <thread.h>
#include <timer.h>

#define PIT_FREQ     1193182
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

/* volatile: changed by the interrupt handler behind the main code's back,
   so the compiler must re-read it from memory every time. */
static volatile uint32_t ticks;

static void timer_irq(struct regs *r)
{
    ticks++;
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
    sched_tick();                       /* next time slot; may switch threads */
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
