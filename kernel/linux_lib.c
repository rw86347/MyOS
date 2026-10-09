/*
 * linux_lib.c — the plain library part of the Linux driver API (the shim):
 * formatted printing (vsnprintf with Linux's %p extensions, printk),
 * strings, number parsing, bitmaps, delays, clocks, WARN/BUG.
 *
 * Written for MyOS against Linux's documented behaviour (the headers in
 * include/linux/ say what each one promises); nothing here is copied.
 */
#include <cycles.h>
#include <interrupts.h>
#include <kprintf.h>
#include <linux/bitops.h>
#include <linux/bsearch.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <net/checksum.h>
#include <serial.h>
#include <stdarg.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>

const char hex_asc[] = "0123456789abcdef";
static const char hex_asc_upper[] = "0123456789ABCDEF";

/* --- vsnprintf ------------------------------------------------------------ */

/* Where the characters go: a buffer of `size`, counting past its end so
   the return value is the length the whole result would have had. */
struct sink {
    char *buf;
    size_t size, len;
};

static void put(struct sink *s, char c)
{
    if (s->len + 1 < s->size)
        s->buf[s->len] = c;
    s->len++;
}

static void puts_n(struct sink *s, const char *str, size_t n)
{
    while (n--)
        put(s, *str++);
}

#define F_LEFT  1
#define F_PLUS  2
#define F_SPACE 4
#define F_ALT   8
#define F_ZERO  16
#define F_UPPER 32

static void number(struct sink *s, unsigned long long v, int neg, unsigned base, int flags,
                   int width, int prec)
{
    char tmp[24];
    int n = 0;
    const char *digits = (flags & F_UPPER) ? hex_asc_upper : hex_asc;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    if (prec < n)
        prec = n;
    char sign = neg ? '-' : (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : 0;
    int prefix = (flags & F_ALT) && base == 16 ? 2 : (flags & F_ALT) && base == 8 ? 1 : 0;
    int total = prec + (sign != 0) + prefix;
    if (!(flags & F_LEFT) && !(flags & F_ZERO))
        for (; width > total; width--)
            put(s, ' ');
    if (sign)
        put(s, sign);
    if (prefix) {
        put(s, '0');
        if (base == 16)
            put(s, (flags & F_UPPER) ? 'X' : 'x');
    }
    if (!(flags & F_LEFT) && (flags & F_ZERO))
        for (; width > total; width--)
            put(s, '0');
    for (int k = n; k < prec; k++)
        put(s, '0');
    while (n)
        put(s, tmp[--n]);
    for (; width > total; width--)
        put(s, ' ');
}

static void string(struct sink *s, const char *str, int flags, int width, int prec)
{
    if (!str)
        str = "(null)";
    size_t n = prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str);
    if (!(flags & F_LEFT))
        for (; width > (int)n; width--)
            put(s, ' ');
    puts_n(s, str, n);
    for (; width > (int)n; width--)
        put(s, ' ');
}


static int vformat(struct sink *s, const char *fmt, va_list ap);

/* Linux's %p extensions: what the pointer points at, printed. `fmt` is just
   past the 'p'; returns how many extra format characters were used. */
static int pointer(struct sink *s, const char *fmt, const void *p, int width, int flags)
{
    const uint8_t *b = p;
    char sep = ':';
    switch (*fmt) {
    case 'M':                           /* %pM: a MAC address, 00:11:22:33:44:55 */
    case 'm':                           /* %pm: the same without colons */
        if (!p) {
            string(s, "(null)", 0, width, -1);
            return 1;
        }
        for (int k = 0; k < 6; k++) {
            if (k && *fmt == 'M')
                put(s, ':');
            put(s, hex_asc_hi(b[k]));
            put(s, hex_asc_lo(b[k]));
        }
        return fmt[1] == 'R' || fmt[1] == 'F' ? 2 : 1;
    case 'I':                           /* %pI4: an IPv4 address in network order */
        if (fmt[1] == '4' && p) {
            for (int k = 0; k < 4; k++) {
                if (k)
                    put(s, '.');
                number(s, b[k], 0, 10, 0, 0, -1);
            }
            return 2;
        }
        break;
    case 'h': {                         /* %*ph: bytes in hex, the count is the width */
        int n = width > 0 ? width : 1;
        int used = 1;
        if (fmt[1] == 'C') { sep = ':'; used = 2; }
        else if (fmt[1] == 'D') { sep = '-'; used = 2; }
        else if (fmt[1] == 'N') { sep = 0; used = 2; }
        else sep = ' ';
        if (!p)
            return used;
        for (int k = 0; k < n && k < 64; k++) {
            if (k && sep)
                put(s, sep);
            put(s, hex_asc_hi(b[k]));
            put(s, hex_asc_lo(b[k]));
        }
        return used;
    }
    case 'V': {                         /* %pV: another format and its arguments */
        const struct va_format *vf = p;
        va_list copy;
        va_copy(copy, *vf->va);
        vformat(s, vf->fmt, copy);
        va_end(copy);
        return 1;
    }
    case 'a':                           /* %pa: a phys_addr_t; %pad: a dma_addr_t */
        if (p) {
            u64 v = fmt[1] == 'd' ? *(const dma_addr_t *)p : (u64)*(const phys_addr_t *)p;
            number(s, v, 0, 16, F_ALT | F_ZERO, 2 + 2 * (int)sizeof(v), -1);
        }
        return fmt[1] == 'd' || fmt[1] == 'p' ? 2 : 1;
    case 'e':                           /* %pe: an ERR_PTR, as its number */
        number(s, (unsigned long long)-(long)p, 1, 10, 0, 0, -1);
        return 1;
    case 'S': case 's': case 'B': case 'F': case 'f':   /* symbols: just the address */
        number(s, (uintptr_t)p, 0, 16, F_ALT, 0, -1);
        return 1;
    }
    (void)flags;
    number(s, (uintptr_t)p, 0, 16, F_ALT, 0, -1);
    return 0;
}

static int vformat(struct sink *s, const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            put(s, *fmt++);
            continue;
        }
        fmt++;
        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_ALT;
            else if (*fmt == '0') flags |= F_ZERO;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_LEFT; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int len = 0;                    /* 1 h, 2 hh, 3 l, 4 ll, 5 z/t, 6 j */
        if (*fmt == 'h') { len = 1; fmt++; if (*fmt == 'h') { len = 2; fmt++; } }
        else if (*fmt == 'l') { len = 3; fmt++; if (*fmt == 'l') { len = 4; fmt++; } }
        else if (*fmt == 'z' || *fmt == 't') { len = 5; fmt++; }
        else if (*fmt == 'j' || *fmt == 'L' || *fmt == 'q') { len = 4; fmt++; }
        char c = *fmt++;
        unsigned long long u;
        long long v;
        switch (c) {
        case 'd':
        case 'i':
            v = len == 4 ? va_arg(ap, long long) : len == 3 ? va_arg(ap, long)
              : len == 5 ? (long long)va_arg(ap, ssize_t) : va_arg(ap, int);
            if (len == 1) v = (short)v;
            if (len == 2) v = (signed char)v;
            number(s, v < 0 ? -(unsigned long long)v : (unsigned long long)v, v < 0, 10,
                   flags, width, prec);
            break;
        case 'u': case 'x': case 'X': case 'o':
            u = len == 4 ? va_arg(ap, unsigned long long) : len == 3 ? va_arg(ap, unsigned long)
              : len == 5 ? va_arg(ap, size_t) : va_arg(ap, unsigned int);
            if (len == 1) u = (unsigned short)u;
            if (len == 2) u = (unsigned char)u;
            if (c == 'X') flags |= F_UPPER;
            number(s, u, 0, c == 'u' ? 10 : c == 'o' ? 8 : 16, flags, width, prec);
            break;
        case 'c':
            put(s, (char)va_arg(ap, int));
            break;
        case 's':
            string(s, va_arg(ap, const char *), flags, width, prec);
            break;
        case 'p':
            fmt += pointer(s, fmt, va_arg(ap, const void *), width, flags);
            break;
        case '%':
            put(s, '%');
            break;
        case 0:
            fmt--;                      /* a lone '%' at the end */
            break;
        default:
            put(s, '%');
            put(s, c);
        }
    }
    return (int)s->len;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s = { buf, size, 0 };
    vformat(&s, fmt, ap);
    if (size)
        buf[s.len < size ? s.len : size - 1] = '\0';
    return (int)s.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vscnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int n = vsnprintf(buf, size, fmt, ap);
    return size == 0 ? 0 : n < (int)size ? n : (int)size - 1;
}

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vscnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)INT_MAX, fmt, ap);
    va_end(ap);
    return n;
}

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = kmalloc((size_t)n + 1, gfp);
    if (p) {
        va_start(ap, fmt);
        vsnprintf(p, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    return p;
}

/* --- printk --------------------------------------------------------------- */

/* Levels 0-4 (emergency to warning) go to the screen and the log, 5 and 6
   (notice, info) to the log only; debug messages are compiled out. */
int printk(const char *fmt, ...)
{
    char buf[512];
    int level = 4;                      /* no level given: a warning, as Linux */
    if (fmt[0] == KERN_SOH[0] && fmt[1]) {
        level = fmt[1] >= '0' && fmt[1] <= '7' ? fmt[1] - '0' : 4;
        fmt += 2;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vscnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (level <= 4)
        kprintf("%s", buf);
    else
        klog("%s", buf);
    return n;
}

/* Linux prints the call stack here. We have no unwinder (the kernel is
   built without frame pointers), so we print who called. */
void dump_stack(void)
{
    printk(KERN_ERR "  called from %p\n", __builtin_return_address(0));
}

void print_hex_dump(const char *level, const char *prefix, int type, int rowsize,
                    int groupsize, const void *buf, unsigned long len, int ascii)
{
    (void)groupsize;
    (void)ascii;
    const uint8_t *b = buf;
    if (rowsize <= 0 || rowsize > 32)
        rowsize = 16;
    for (unsigned long off = 0; off < len; off += (unsigned long)rowsize) {
        char line[128];
        int n = scnprintf(line, sizeof line, "%s", prefix ? prefix : "");
        if (type == DUMP_PREFIX_OFFSET)
            n += scnprintf(line + n, sizeof line - n, "%08lx: ", off);
        for (unsigned long k = off; k < len && k < off + (unsigned long)rowsize; k++)
            n += scnprintf(line + n, sizeof line - n, "%02x ", b[k]);
        printk("%s%s\n", level ? level : KERN_DEBUG, line);
    }
}

/* --- strings -------------------------------------------------------------- */







/* Copy with a NUL always written: the length, or -E2BIG if src was cut. */
ssize_t strscpy(char *dst, const char *src, size_t size)
{
    if (!size)
        return -E2BIG;
    size_t n = strnlen(src, size);
    if (n == size) {
        memcpy(dst, src, size - 1);
        dst[size - 1] = '\0';
        return -E2BIG;
    }
    memcpy(dst, src, n + 1);
    return (ssize_t)n;
}







/* The first byte that is NOT c, or NULL if all are. */
void *memchr_inv(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p != (unsigned char)c)
            return (void *)p;
    return NULL;
}

int sysfs_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    if (*a == *b)
        return 1;
    if (!*a && *b == '\n' && !b[1])
        return 1;
    return !*b && *a == '\n' && !a[1];
}

int match_string(const char *const *array, size_t n, const char *s)
{
    for (size_t k = 0; k < n && array[k]; k++)
        if (!strcmp(array[k], s))
            return (int)k;
    return -EINVAL;
}

/* --- numbers -------------------------------------------------------------- */

/* Linux's rules: optional sign (signed only), 0x/0 prefixes when base is
   0 (or 16 with 0x), digits, at most one trailing newline. */
static int parse_ull(const char *s, unsigned int base, unsigned long long *res)
{
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0' && s[1]) { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    unsigned long long v = 0;
    int digits = 0;
    for (;; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10
              : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99;
        if (d >= (int)base)
            break;
        if (v > (~0ULL - (unsigned)d) / base)
            return -ERANGE;
        v = v * base + (unsigned)d;
        digits++;
    }
    if (*s == '\n')
        s++;
    if (!digits || *s)
        return -EINVAL;
    *res = v;
    return 0;
}

static int parse_ll(const char *s, unsigned int base, long long *res)
{
    int neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    unsigned long long u;
    int err = parse_ull(s, base, &u);
    if (err)
        return err;
    if (u > (unsigned long long)LLONG_MAX + neg)
        return -ERANGE;
    *res = neg ? -(long long)u : (long long)u;
    return 0;
}

#define KSTRTO_U(name, type, maxv)                                     \
    int name(const char *s, unsigned int base, type *res)              \
    {                                                                  \
        unsigned long long v;                                          \
        int err = parse_ull(*s == '+' ? s + 1 : s, base, &v);          \
        if (err)                                                       \
            return err;                                                \
        if (v > (maxv))                                                \
            return -ERANGE;                                            \
        *res = (type)v;                                                \
        return 0;                                                      \
    }
KSTRTO_U(kstrtoul, unsigned long, ULONG_MAX)
KSTRTO_U(kstrtouint, unsigned int, UINT_MAX)
KSTRTO_U(kstrtou32, u32, U32_MAX)
KSTRTO_U(kstrtou16, u16, U16_MAX)
KSTRTO_U(kstrtou8, u8, U8_MAX)

int kstrtol(const char *s, unsigned int base, long *res)
{
    long long v;
    int err = parse_ll(s, base, &v);
    if (err)
        return err;
    if (v > LONG_MAX || v < -LONG_MAX - 1)
        return -ERANGE;
    *res = (long)v;
    return 0;
}

int kstrtoint(const char *s, unsigned int base, int *res)
{
    long long v;
    int err = parse_ll(s, base, &v);
    if (err)
        return err;
    if (v > INT_MAX || v < INT_MIN)
        return -ERANGE;
    *res = (int)v;
    return 0;
}

int kstrtobool(const char *s, bool *res)
{
    if (!s)
        return -EINVAL;
    switch (s[0]) {
    case 'y': case 'Y': case 't': case 'T': case '1':
        *res = true;
        return 0;
    case 'n': case 'N': case 'f': case 'F': case '0':
        *res = false;
        return 0;
    case 'o': case 'O':
        if (s[1] == 'n' || s[1] == 'N') { *res = true; return 0; }
        if (s[1] == 'f' || s[1] == 'F') { *res = false; return 0; }
    }
    return -EINVAL;
}

int hex_to_bin(unsigned char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    ch |= 0x20;
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

int hex2bin(u8 *dst, const char *src, size_t count)
{
    while (count--) {
        int hi = hex_to_bin((unsigned char)*src++), lo = hex_to_bin((unsigned char)*src++);
        if (hi < 0 || lo < 0)
            return -EINVAL;
        *dst++ = (u8)(hi << 4 | lo);
    }
    return 0;
}

char *bin2hex(char *dst, const void *src, size_t count)
{
    const u8 *b = src;
    while (count--) {
        *dst++ = hex_asc_hi(*b);
        *dst++ = hex_asc_lo(*b);
        b++;
    }
    return dst;
}

/* a * b / c with a 128-bit middle: long multiplication into two 64-bit
   halves, then long division (i386 has no 128-bit type). */
u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c)
{
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 lo = al * bl, m1 = al * bh, m2 = ah * bl, hi = ah * bh;
    u64 mid = (lo >> 32) + (u32)m1 + (u32)m2;
    lo = (lo & 0xffffffffu) | (mid << 32);
    hi += (m1 >> 32) + (m2 >> 32) + (mid >> 32);
    /* Long division of (hi:lo) by c, one bit at a time from the top: the
       remainder takes the next bit, and c goes into it at most once. */
    u64 r = 0, q = 0;
    for (int i = 127; i >= 0; i--) {
        u64 bit = i >= 64 ? (hi >> (i - 64)) & 1 : (lo >> i) & 1;
        u64 top = r >> 63;
        r = r << 1 | bit;
        q <<= 1;
        if (top || r >= c) {
            r -= c;
            q |= 1;
        }
    }
    return q;
}

/* --- bitmaps ---------------------------------------------------------------- */

unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long from)
{
    while (from < size) {
        unsigned long w = a[BIT_WORD(from)] >> (from % BITS_PER_LONG);
        if (w)
            return min(from + __ffs(w), size);
        from = (BIT_WORD(from) + 1) * BITS_PER_LONG;
    }
    return size;
}

unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long from)
{
    while (from < size) {
        unsigned long w = ~a[BIT_WORD(from)] >> (from % BITS_PER_LONG);
        if (w)
            return min(from + __ffs(w), size);
        from = (BIT_WORD(from) + 1) * BITS_PER_LONG;
    }
    return size;
}

void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
    memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(long));
}

void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
    memset(dst, 0xff, BITS_TO_LONGS(nbits) * sizeof(long));
}

void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
    memcpy(dst, src, BITS_TO_LONGS(nbits) * sizeof(long));
}

int bitmap_empty(const unsigned long *src, unsigned int nbits)
{
    return find_first_bit(src, nbits) >= nbits;
}

int bitmap_full(const unsigned long *src, unsigned int nbits)
{
    return find_first_zero_bit(src, nbits) >= nbits;
}

int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
    int n = 0;
    for (unsigned int k = 0; k < nbits; k++)
        n += test_bit(k, src);
    return n;
}

int bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
    for (unsigned int k = 0; k < nbits; k++)
        if (test_bit(k, a) != test_bit(k, b))
            return 0;
    return 1;
}

void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
    for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++)
        dst[k] = a[k] | b[k];
}

int bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits)
{
    unsigned long any = 0;
    for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++)
        any |= dst[k] = a[k] & b[k];
    return any != 0;
}

/* --- time and delays -------------------------------------------------------- */

/* Nanoseconds since boot from the cycle counter, split so the multiply
   can't overflow: whole microseconds, then the remainder. */
u64 ktime_get_ns(void)
{
    u64 c = cycles_now();
    u64 mhz = cycles_per_us();
    return c / mhz * 1000 + c % mhz * 1000 / mhz;
}

ktime_t ktime_get_real(void)
{
    return clock_ns(0);                 /* CLOCK_REALTIME */
}

void udelay(unsigned long us)
{
    u64 end = cycles_now() + (u64)us * cycles_per_us();
    while (cycles_now() < end)
        __asm__ volatile ("pause");
}

void ndelay(unsigned long ns)
{
    udelay((ns + 999) / 1000);
}

/* Sleep at least ms: the tick is 1 ms, so one more tick covers the part of
   the current one that has already gone. */
void msleep(unsigned int ms)
{
    struct wait_queue q = { NULL };
    unsigned long f = irq_save();
    uint32_t end = timer_ms() + ms + 1;
    while ((int32_t)(timer_ms() - end) < 0)
        wq_sleep_until(&q, end ? end : 1);
    irq_restore(f);
}

unsigned long msleep_interruptible(unsigned int ms)
{
    msleep(ms);
    return 0;
}

void usleep_range(unsigned long min, unsigned long max)
{
    (void)max;
    if (min < 1000)
        udelay(min);                    /* shorter than a tick: spin */
    else
        msleep((unsigned int)((min + 999) / 1000));
}

/* --- searching, CRC-32, checksums ------------------------------------------------ */

/* The element of a sorted array that cmp says equals key, or NULL. */
void *bsearch(const void *key, const void *base, size_t num, size_t size,
              int (*cmp)(const void *key, const void *elt))
{
    const char *lo = base;
    while (num) {
        const char *mid = lo + (num / 2) * size;
        int r = cmp(key, mid);
        if (!r)
            return (void *)mid;
        if (r > 0) {
            lo = mid + size;
            num -= num / 2 + 1;
        } else {
            num /= 2;
        }
    }
    return NULL;
}

/* CRC-32 as Ethernet computes it (polynomial 0xEDB88320, least significant
   bit first), Linux's crc32_le: no inversion before or after — callers
   start from ~0 and invert the result themselves. Four bits at a time from
   a 16-entry table: 64 bytes of cache instead of 1 KiB, and the buffers
   drivers check are short. */
static const u32 crc_nibble[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

u32 crc32_le(u32 crc, const void *p, size_t len)
{
    const u8 *b = p;
    while (len--) {
        crc ^= *b++;
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
    }
    return crc;
}

/* The Internet checksum's running sum (RFC 1071) of buf, added to sum:
   16-bit words as they lie in memory, the carries folded back in. Not yet
   complemented (csum_fold does that). */
__wsum csum_partial(const void *buf, int len, __wsum sum)
{
    const u8 *p = buf;
    u64 s = (u32)sum;
    for (; len > 1; p += 2, len -= 2) {
        u16 w;
        memcpy(&w, p, 2);
        s += w;
    }
    if (len) {
        u16 w = 0;
        memcpy(&w, p, 1);               /* the last byte, as the first of a word */
        s += w;
    }
    while (s >> 32)
        s = (s & 0xffffffffu) + (s >> 32);
    return (__wsum)s;
}

/* --- WARN and BUG ------------------------------------------------------------- */

void linux_warn(const char *file, int line, const char *what)
{
    kprintf("WARNING: %s:%d: %s\n", file, line, what);
}

void linux_bug(const char *file, int line, const char *what)
{
    kprintf("\n*** kernel BUG at %s:%d: %s ***\n", file, line, what);
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}
