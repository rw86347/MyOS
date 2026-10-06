#ifndef LINUX_BITOPS_H
#define LINUX_BITOPS_H

/* Bits: masks, finding the first set bit, and bitmaps (arrays of longs).
   set_bit and friends are atomic in Linux (another CPU or an interrupt may
   touch the same word); ours use the CPU's locked instructions too, so an
   interrupt handler sharing the word is safe. The __ versions are plain. */
#include <linux/compiler.h>
#include <linux/types.h>

#define BITS_PER_LONG        (8 * (int)sizeof(long))
#define BITS_PER_BYTE        8
#define BIT(n)               (1UL << (n))
#define BIT_ULL(n)           (1ULL << (n))
#define BIT_MASK(n)          (1UL << ((n) % BITS_PER_LONG))
#define BIT_WORD(n)          ((n) / BITS_PER_LONG)
#define BITS_TO_LONGS(n)     (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define GENMASK(h, l)        (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l)    (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (63 - (h))))
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

static inline void set_bit(long n, volatile unsigned long *a)
{ __atomic_or_fetch(&a[BIT_WORD(n)], BIT_MASK(n), __ATOMIC_SEQ_CST); }
static inline void clear_bit(long n, volatile unsigned long *a)
{ __atomic_and_fetch(&a[BIT_WORD(n)], ~BIT_MASK(n), __ATOMIC_SEQ_CST); }
static inline void change_bit(long n, volatile unsigned long *a)
{ __atomic_xor_fetch(&a[BIT_WORD(n)], BIT_MASK(n), __ATOMIC_SEQ_CST); }
static inline int test_and_set_bit(long n, volatile unsigned long *a)
{ return (__atomic_fetch_or(&a[BIT_WORD(n)], BIT_MASK(n), __ATOMIC_SEQ_CST) & BIT_MASK(n)) != 0; }
static inline int test_and_clear_bit(long n, volatile unsigned long *a)
{ return (__atomic_fetch_and(&a[BIT_WORD(n)], ~BIT_MASK(n), __ATOMIC_SEQ_CST) & BIT_MASK(n)) != 0; }
static inline void __set_bit(long n, volatile unsigned long *a) { a[BIT_WORD(n)] |= BIT_MASK(n); }
static inline void __clear_bit(long n, volatile unsigned long *a) { a[BIT_WORD(n)] &= ~BIT_MASK(n); }
static inline int __test_and_set_bit(long n, volatile unsigned long *a)
{ int old = (a[BIT_WORD(n)] & BIT_MASK(n)) != 0; a[BIT_WORD(n)] |= BIT_MASK(n); return old; }
static inline int __test_and_clear_bit(long n, volatile unsigned long *a)
{ int old = (a[BIT_WORD(n)] & BIT_MASK(n)) != 0; a[BIT_WORD(n)] &= ~BIT_MASK(n); return old; }
static inline int test_bit(long n, const volatile unsigned long *a)
{ return (a[BIT_WORD(n)] >> (n % BITS_PER_LONG)) & 1; }
#define assign_bit(n, a, v)   ((v) ? set_bit(n, a) : clear_bit(n, a))
#define __assign_bit(n, a, v) ((v) ? __set_bit(n, a) : __clear_bit(n, a))

static inline int ffs(int x) { return __builtin_ffs(x); }               /* 1-based; 0: none */
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned long __ffs(unsigned long x) { return (unsigned long)__builtin_ctzl(x); }
static inline unsigned long __fls(unsigned long x) { return BITS_PER_LONG - 1 - __builtin_clzl(x); }
static inline unsigned long ffz(unsigned long x) { return __ffs(~x); }
#define HWEIGHT8(w)           __builtin_popcount((u8)(w))
#define HWEIGHT32(w)          __builtin_popcount((u32)(w))
#define HWEIGHT64(w)          __builtin_popcountll((u64)(w))
static inline unsigned int hweight8(unsigned int w) { return __builtin_popcount(w & 0xff); }
static inline unsigned int hweight16(unsigned int w) { return __builtin_popcount(w & 0xffff); }
static inline unsigned int hweight32(unsigned int w) { return __builtin_popcount(w); }
static inline unsigned long hweight64(u64 w) { return __builtin_popcountll(w); }
static inline unsigned long hweight_long(unsigned long w) { return __builtin_popcountl(w); }
#define ilog2(n)              ((n) ? 63 - __builtin_clzll((u64)(n)) : -1)   /* constant for constants */
#define order_base_2(n)       ((n) > 1 ? ilog2((n) - 1) + 1 : 0)
/* A macro, not a function, so a constant argument gives a constant (an
   alignment may be written with it). */
#define roundup_pow_of_two(n) ((unsigned long)(n) <= 1 ? 1UL : 1UL << (64 - __builtin_clzll((u64)(n) - 1)))
static inline unsigned long rounddown_pow_of_two(unsigned long n) { return n ? 1UL << (fls64((u64)n) - 1) : 0; }

/* Bitmaps (linux_lib.c) */
unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long from);
unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long from);
#define find_first_bit(a, size)      find_next_bit(a, size, 0)
#define find_first_zero_bit(a, size) find_next_zero_bit(a, size, 0)
#define for_each_set_bit(bit, a, size) \
    for ((bit) = find_first_bit(a, size); (bit) < (size); (bit) = find_next_bit(a, size, (bit) + 1))
#define for_each_clear_bit(bit, a, size) \
    for ((bit) = find_first_zero_bit(a, size); (bit) < (size); (bit) = find_next_zero_bit(a, size, (bit) + 1))
void bitmap_zero(unsigned long *dst, unsigned int nbits);
void bitmap_fill(unsigned long *dst, unsigned int nbits);
void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits);
int  bitmap_empty(const unsigned long *src, unsigned int nbits);
int  bitmap_full(const unsigned long *src, unsigned int nbits);
int  bitmap_weight(const unsigned long *src, unsigned int nbits);
int  bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned int nbits);
void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);
int  bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);

#endif
