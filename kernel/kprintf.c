/*
 * kprintf.c — a small printf for the kernel.
 *
 * Variadic functions: <stdarg.h> is one of the headers the compiler itself
 * provides, so it works even in a freestanding kernel. va_arg() walks
 * through the extra arguments one by one.
 */
#include <stdarg.h>
#include <stdint.h>
#include <kprintf.h>
#include <serial.h>
#include <string.h>
#include <vga.h>

static void out(char c)
{
    vga_putc(c);
    serial_putc(c);
}

/* Print `s` padded to `width` (pad on the left unless left_align). */
static void out_padded(const char *s, int width, char pad, int left_align)
{
    int len = (int)strlen(s);
    if (!left_align)
        for (; width > len; width--)
            out(pad);
    while (*s)
        out(*s++);
    for (; width > len; width--)
        out(' ');
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

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(*fmt);
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
            out_padded(s ? s : "(null)", width, ' ', left_align);
            break;
        }
        case 'c':
            out((char)va_arg(ap, int));
            break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                buf[0] = '-';
                utoa((uint32_t)-(int64_t)v, 10, buf + 1);
            } else {
                utoa((uint32_t)v, 10, buf);
            }
            out_padded(buf, width, pad, left_align);
            break;
        }
        case 'u':
            utoa(va_arg(ap, uint32_t), 10, buf);
            out_padded(buf, width, pad, left_align);
            break;
        case 'x':
            utoa(va_arg(ap, uint32_t), 16, buf);
            out_padded(buf, width, pad, left_align);
            break;
        case 'p':
            out('0'); out('x');
            utoa((uint32_t)(uintptr_t)va_arg(ap, void *), 16, buf);
            out_padded(buf, 8, '0', 0);
            break;
        case '%':
            out('%');
            break;
        default:            /* unknown: print it literally */
            out('%');
            out(*fmt);
            break;
        }
    }
    va_end(ap);
}
