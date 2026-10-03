/*
 * kprintf.c — a small printf for the kernel.
 *
 * Variadic functions: <stdarg.h> is one of the headers the compiler itself
 * provides, so it works even in a freestanding kernel. va_arg() walks
 * through the extra arguments one by one.
 *
 * Speed: the text is collected in a small buffer on the stack and handed to
 * the console in one piece (console_write), not one character at a time.
 * Each hand-over costs one interrupt-off section and one cursor move —
 * port I/O is slow on old PCs, so per-character work adds up.
 */
#include <stdarg.h>
#include <stdint.h>
#include <interrupts.h>
#include <kprintf.h>
#include <serial.h>
#include <string.h>
#include <vt.h>

void console_write(const char *s, size_t n)
{
    /* Interrupts off for the whole piece: output from two threads (or an
       interrupt) can't get mixed up inside one line. */
    uint32_t flags = irq_save();
    vt_write(s, n);
    serial_write_n(s, n);
    irq_restore(flags);
}

/* Where output goes: kprintf -> screen + serial, klog -> serial only. */
struct out {
    void (*flush)(const char *s, size_t n);
    size_t n;
    char buf[120];
};

static void out(struct out *o, char c)
{
    o->buf[o->n++] = c;
    if (o->n == sizeof o->buf) {
        o->flush(o->buf, o->n);
        o->n = 0;
    }
}

/* Print `s` padded to `width` (pad on the left unless left_align). */
static void out_padded(struct out *o, const char *s, int width, char pad, int left_align)
{
    int len = (int)strlen(s);
    if (!left_align)
        for (; width > len; width--)
            out(o, pad);
    while (*s)
        out(o, *s++);
    for (; width > len; width--)
        out(o, ' ');
}

/* Convert an unsigned number to text in the given base (10 or 16). */
static void utoa(uint32_t v, unsigned base, char *buf)
{
    char tmp[12];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (i)
        *buf++ = tmp[--i];     /* digits came out backwards; reverse them */
    *buf = '\0';
}

static void vformat(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(o, *fmt);
            continue;
        }
        fmt++;

        /* Flags and width, e.g. "%-12s" or "%08x". */
        int left_align = 0;
        char pad = ' ';
        int width = 0;
        if (*fmt == '-') { left_align = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        /* 'l' (long), as in %lu. On i386, long and int are both 32 bits, so
           it changes nothing — but uint32_t is "unsigned long" with this
           compiler, so %lu is what the compiler's format checker expects. */
        while (*fmt == 'l')
            fmt++;
        if (!*fmt)
            break;          /* format string ended in the middle of a % */

        char buf[16];
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            out_padded(o, s ? s : "(null)", width, ' ', left_align);
            break;
        }
        case 'c':
            out(o, (char)va_arg(ap, int));
            break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                buf[0] = '-';
                utoa((uint32_t)-(int64_t)v, 10, buf + 1);
            } else {
                utoa((uint32_t)v, 10, buf);
            }
            out_padded(o, buf, width, pad, left_align);
            break;
        }
        case 'u':
            utoa(va_arg(ap, uint32_t), 10, buf);
            out_padded(o, buf, width, pad, left_align);
            break;
        case 'x':
            utoa(va_arg(ap, uint32_t), 16, buf);
            out_padded(o, buf, width, pad, left_align);
            break;
        case 'p':
            out(o, '0'); out(o, 'x');
            utoa((uint32_t)(uintptr_t)va_arg(ap, void *), 16, buf);
            out_padded(o, buf, 8, '0', 0);
            break;
        case '%':
            out(o, '%');
            break;
        default:            /* unknown: print it literally */
            out(o, '%');
            out(o, *fmt);
            break;
        }
    }
    if (o->n)
        o->flush(o->buf, o->n);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct out o = { console_write, 0, {0} };
    vformat(&o, fmt, ap);
    va_end(ap);
}

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct out o = { serial_write_n, 0, {0} };
    vformat(&o, fmt, ap);
    va_end(ap);
}
