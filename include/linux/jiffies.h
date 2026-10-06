#ifndef LINUX_JIFFIES_H
#define LINUX_JIFFIES_H

/* Linux counts time in "jiffies", timer ticks. Ours tick every 1 ms
   (HZ = 1000), so a jiffy is a millisecond. `jiffies` is a real variable,
   as in Linux, counted by the tick (drivers/pit.c): 32 bits on i386 (it
   wraps after 49 days, and time_after() is written to survive that), 64
   on x86_64. */
#include <linux/types.h>

#define NSEC_PER_USEC 1000L
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_SEC  1000000000L
#define USEC_PER_MSEC 1000L
#define USEC_PER_SEC  1000000L
#define MSEC_PER_SEC  1000L

#define HZ 1000
extern volatile unsigned long jiffies;
static inline u64 get_jiffies_64(void) { return jiffies; }

#define time_after(a, b)          ((long)((b) - (a)) < 0)
#define time_before(a, b)         time_after(b, a)
#define time_after_eq(a, b)       ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)      time_after_eq(b, a)
#define time_in_range(a, b, c)    (time_after_eq(a, b) && time_before_eq(a, c))
#define time_is_before_jiffies(a) time_after(jiffies, a)
#define time_is_after_jiffies(a)  time_before(jiffies, a)
#define time_is_before_eq_jiffies(a) time_after_eq(jiffies, a)
#define time_is_after_eq_jiffies(a)  time_before_eq(jiffies, a)
#define MAX_JIFFY_OFFSET          ((long)(~0UL >> 1) - 1)

static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }
static inline unsigned long usecs_to_jiffies(unsigned int u) { return (u + 999) / 1000; }
static inline unsigned long nsecs_to_jiffies(u64 n) { return (unsigned long)((n + 999999) / 1000000); }
static inline unsigned int jiffies_to_msecs(unsigned long j) { return (unsigned int)j; }
static inline unsigned int jiffies_to_usecs(unsigned long j) { return (unsigned int)(j * 1000); }
static inline u64 jiffies_to_nsecs(unsigned long j) { return (u64)j * 1000000; }
#define secs_to_jiffies(s)        ((unsigned long)(s) * HZ)
#define round_jiffies(j)          (j)
#define round_jiffies_relative(j) (j)
#define round_jiffies_up(j)       (j)
#define round_jiffies_up_relative(j) (j)

#endif
