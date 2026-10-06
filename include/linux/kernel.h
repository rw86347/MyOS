#ifndef LINUX_KERNEL_H
#define LINUX_KERNEL_H

/* The everyday helpers of Linux's kernel.h: min/max, array sizes, rounding,
   bit masks, container_of, and the printk family (printk.h). */
#include <linux/bitops.h>
#include <linux/byteorder.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/overflow.h>
#include <linux/cache.h>
#include <linux/printk.h>
#include <linux/types.h>

#define ARRAY_SIZE(a)            (sizeof(a) / sizeof((a)[0]))
#define min(a, b)                ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b)                ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b)           ({ t _a = (t)(a); t _b = (t)(b); _a < _b ? _a : _b; })
#define max_t(t, a, b)           ({ t _a = (t)(a); t _b = (t)(b); _a > _b ? _a : _b; })
#define min3(a, b, c)            min(min(a, b), c)
#define max3(a, b, c)            max(max(a, b), c)
#define clamp(v, lo, hi)         min(max(v, lo), hi)
#define clamp_t(t, v, lo, hi)    min_t(t, max_t(t, v, lo), hi)
#define clamp_val(v, lo, hi)     clamp_t(__typeof__(v), v, lo, hi)
#define swap(a, b)               do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define abs(x)                   ({ __typeof__(x) _x = (x); _x < 0 ? -_x : _x; })
#define DIV_ROUND_UP(n, d)       (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(n, d)  (((n) + (d) / 2) / (d))
#define roundup(x, y)            ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y)          ((x) - ((x) % (y)))
#define round_up(x, y)           ((((x) - 1) | ((__typeof__(x))((y) - 1))) + 1)
#define round_down(x, y)         ((x) & ~((__typeof__(x))((y) - 1)))
#define ALIGN(x, a)              (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a)         ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)         (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a)          ((__typeof__(p))ALIGN((unsigned long)(p), (a)))
#define upper_32_bits(n)         ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n)         ((u32)((n) & 0xffffffff))
#define U8_MAX   ((u8)~0U)
#define U16_MAX  ((u16)~0U)
#define U32_MAX  ((u32)~0U)
#define U64_MAX  ((u64)~0ULL)
#define S8_MAX   ((s8)(U8_MAX >> 1))
#define S8_MIN   ((s8)(-S8_MAX - 1))
#define S16_MIN  ((s16)(-S16_MAX - 1))
#define S16_MAX  ((s16)(U16_MAX >> 1))
#define S32_MAX  ((s32)(U32_MAX >> 1))
#define S32_MIN  ((s32)(-S32_MAX - 1))
#define S64_MAX  ((s64)(U64_MAX >> 1))
#define INT_MAX  ((int)(~0U >> 1))
#define INT_MIN  (-INT_MAX - 1)
#define UINT_MAX (~0U)
#define LONG_MAX ((long)(~0UL >> 1))
#define ULONG_MAX (~0UL)
#define LLONG_MAX ((long long)(~0ULL >> 1))
#define ULLONG_MAX (~0ULL)
#define typeof_member(t, m)      __typeof__(((t *)0)->m)
#define is_power_of_2(n)         ((n) != 0 && (((n) & ((n) - 1)) == 0))
#define do_div(n, base)          ({ u32 _rem = (u32)((n) % (base)); (n) /= (base); _rem; })
static inline u64 div_u64(u64 n, u32 d) { return n / d; }
static inline s64 div_s64(s64 n, s32 d) { return n / d; }
static inline u64 div64_u64(u64 n, u64 d) { return n / d; }
u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c);      /* a * b / c without overflow */

/* Linux's WARN_ON/BUG_ON. A warning prints where it happened and goes on;
   a BUG stops the kernel (it means memory may already be wrong). */
void linux_warn(const char *file, int line, const char *what);
void linux_bug(const char *file, int line, const char *what) __attribute__((noreturn));
#define WARN_ON(c)        ({ int _c = !!(c); if (unlikely(_c)) linux_warn(__FILE__, __LINE__, #c); unlikely(_c); })
#define WARN_ON_ONCE(c)   ({ static int _w; int _c = !!(c); if (unlikely(_c) && !_w) { _w = 1; linux_warn(__FILE__, __LINE__, #c); } unlikely(_c); })
#define WARN(c, fmt, ...) ({ int _c = !!(c); if (unlikely(_c)) { pr_warn(fmt, ##__VA_ARGS__); linux_warn(__FILE__, __LINE__, #c); } unlikely(_c); })
#define WARN_ONCE(c, fmt, ...) ({ static int _w; int _c = !!(c); if (unlikely(_c) && !_w) { _w = 1; pr_warn(fmt, ##__VA_ARGS__); } unlikely(_c); })
#define BUG()             linux_bug(__FILE__, __LINE__, "BUG")
#define BUG_ON(c)         do { if (unlikely(c)) linux_bug(__FILE__, __LINE__, #c); } while (0)
#define might_sleep()     do { } while (0)
#define cond_resched()    do { } while (0)

/* Strings and numbers (linux_lib.c) */
int  vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);
int  scnprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);
int  vscnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int  sprintf(char *buf, const char *fmt, ...) __printf(2, 3);
char *kasprintf(gfp_t gfp, const char *fmt, ...) __printf(2, 3);
int  kstrtoul(const char *s, unsigned int base, unsigned long *res);
int  kstrtol(const char *s, unsigned int base, long *res);
int  kstrtouint(const char *s, unsigned int base, unsigned int *res);
int  kstrtoint(const char *s, unsigned int base, int *res);
int  kstrtou8(const char *s, unsigned int base, u8 *res);
int  kstrtou16(const char *s, unsigned int base, u16 *res);
int  kstrtou32(const char *s, unsigned int base, u32 *res);
int  kstrtobool(const char *s, bool *res);
int  hex_to_bin(unsigned char ch);
int  hex2bin(u8 *dst, const char *src, size_t count);
char *bin2hex(char *dst, const void *src, size_t count);
extern const char hex_asc[];
#define hex_asc_lo(x) hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x) hex_asc[((x) & 0xf0) >> 4]

#endif
