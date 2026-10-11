/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_KTIME_H
#define LINUX_KTIME_H

/* ktime_t: nanoseconds, 64 bits. Read from the CPU's cycle counter, so it
   is finer than the 1 ms tick (linux_lib.c). */
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/types.h>

struct timespec64 { s64 tv_sec; long tv_nsec; };

u64 ktime_get_ns(void);             /* since boot */
static inline ktime_t ktime_get(void) { return (ktime_t)ktime_get_ns(); }
#define ktime_get_boottime()       ktime_get()
#define ktime_get_boottime_ns()    ktime_get_ns()
#define ktime_get_raw_ns()         ktime_get_ns()
#define ktime_get_mono_fast_ns()   ktime_get_ns()
#define ktime_get_boottime_seconds() ((s64)(ktime_get_ns() / NSEC_PER_SEC))
#define ktime_get_seconds()        ((s64)(ktime_get_ns() / NSEC_PER_SEC))
ktime_t ktime_get_real(void);       /* since 1970 */
#define ktime_get_real_ns()        ((u64)ktime_get_real())
#define ktime_get_real_seconds()   ((s64)(ktime_get_real() / NSEC_PER_SEC))
#define ktime_set(s, ns)           ((ktime_t)(s) * NSEC_PER_SEC + (ns))
#define ktime_add(a, b)            ((a) + (b))
#define ktime_sub(a, b)            ((a) - (b))
#define ktime_add_ns(a, n)         ((a) + (ktime_t)(n))
#define ktime_add_us(a, u)         ((a) + (ktime_t)(u) * NSEC_PER_USEC)
#define ktime_add_ms(a, m)         ((a) + (ktime_t)(m) * NSEC_PER_MSEC)
#define ktime_to_ns(t)             ((s64)(t))
#define ktime_to_us(t)             ((s64)(t) / NSEC_PER_USEC)
#define ktime_to_ms(t)             ((s64)(t) / NSEC_PER_MSEC)
#define ns_to_ktime(n)             ((ktime_t)(n))
#define ms_to_ktime(m)             ((ktime_t)(m) * NSEC_PER_MSEC)
#define us_to_ktime(u)             ((ktime_t)(u) * NSEC_PER_USEC)
#define ktime_compare(a, b)        ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)
#define ktime_after(a, b)          ((a) > (b))
#define ktime_before(a, b)         ((a) < (b))
#define ktime_us_delta(a, b)       ktime_to_us((a) - (b))
#define ktime_ms_delta(a, b)       ktime_to_ms((a) - (b))
static inline struct timespec64 ns_to_timespec64(s64 ns)
{
    struct timespec64 t = { ns / NSEC_PER_SEC, (long)(ns % NSEC_PER_SEC) };
    if (t.tv_nsec < 0) { t.tv_sec--; t.tv_nsec += NSEC_PER_SEC; }
    return t;
}
#define ktime_to_timespec64(t)     ns_to_timespec64(t)

#endif
