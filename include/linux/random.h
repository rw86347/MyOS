#ifndef LINUX_RANDOM_H
#define LINUX_RANDOM_H

/* Random numbers for drivers, from the kernel's CSPRNG (kernel/random.c). */
#include <linux/types.h>

void get_random_bytes(void *buf, size_t n);
u32 get_random_u32(void);
static inline u8 get_random_u8(void) { return (u8)get_random_u32(); }
static inline u16 get_random_u16(void) { return (u16)get_random_u32(); }
static inline u64 get_random_u64(void) { return (u64)get_random_u32() << 32 | get_random_u32(); }
static inline u32 get_random_u32_below(u32 ceil) { return ceil ? get_random_u32() % ceil : 0; }
static inline u32 get_random_u32_inclusive(u32 lo, u32 hi) { return lo + get_random_u32_below(hi - lo + 1); }
#define prandom_u32_max(c) get_random_u32_below(c)

#endif
