#ifndef ARCH_I386_CYCLES_H
#define ARCH_I386_CYCLES_H

#include <stdint.h>

/*
 * A cycle counter, for measuring how long things take.
 *
 * Every Pentium has the Time-Stamp Counter: a 64-bit register that counts
 * CPU clock cycles since power-on. `rdtsc` reads it in a few dozen cycles —
 * cheap enough to time even a single system call. Cycles are the honest unit
 * for comparing code: they don't depend on the clock speed.
 *
 * (Other architectures will provide this same header with their own
 * counter.)
 */

static inline uint64_t cycles_now(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Called by the timer interrupt every 1 ms: measures the counter's speed
   in the background during the first 12 ms after boot. */
void cycles_tick(uint32_t ms);

/* Counter ticks per microsecond (= the CPU's MHz). Waits for the
   calibration if it isn't finished yet. */
uint32_t cycles_per_us(void);

#endif
