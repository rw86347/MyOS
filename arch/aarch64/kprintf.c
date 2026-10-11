/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * kprintf.c — formatted output to PL011 (ARM64 bring-up; full console later)
 */
#include <kprintf.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "uart.h"

void console_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];

        if (c == '\n')
            uart_putc('\r');
        uart_putc(c);
    }
}

struct out {
    size_t n;
    char buf[120];
};

static void flush_out(struct out *o)
{
    if (o->n) {
        console_write(o->buf, o->n);
        o->n = 0;
    }
}

static void put(struct out *o, char c)
{
    o->buf[o->n++] = c;
    if (o->n == sizeof o->buf)
        flush_out(o);
}

static void utoa(unsigned long v, unsigned base, char *buf)
{
    char tmp[24];
    int i = 0;

    do {
        tmp[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (i)
        *buf++ = tmp[--i];
    *buf = '\0';
}

static void ulltoa(unsigned long long v, unsigned base, char *buf)
{
    if (v == (unsigned long)v) {
        utoa((unsigned long)v, base, buf);
        return;
    }
    char tmp[24];
    int i = 0;

    do {
        tmp[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (i)
        *buf++ = tmp[--i];
    *buf = '\0';
}

#define NEXT_U(ap, longs)                                                                      \
    ((longs) >= 2 ? va_arg(ap, unsigned long long) : (longs) == 1 ? va_arg(ap, unsigned long) \
                                                                  : (unsigned long long)va_arg(ap, unsigned int))

static void out_padded(struct out *o, const char *s, int width, char pad, int left_align)
{
    int len = (int)strlen(s);

    if (!left_align)
        for (; width > len; width--)
            put(o, pad);
    while (*s)
        put(o, *s++);
    for (; width > len; width--)
        put(o, ' ');
}

static void vformat(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(o, *fmt);
            continue;
        }
        fmt++;
        int left_align = 0;
        char pad = ' ';
        int width = 0;

        if (*fmt == '-') {
            left_align = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        int longs = 0;

        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (!*fmt)
            break;

        char buf[24];
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);

            out_padded(o, s ? s : "(null)", width, ' ', left_align);
            break;
        }
        case 'c':
            put(o, (char)va_arg(ap, int));
            break;
        case 'd': {
            long long v = longs >= 2 ? va_arg(ap, long long)
                        : longs == 1 ? va_arg(ap, long)
                                     : va_arg(ap, int);
            char *p = buf;

            if (v < 0) {
                *p++ = '-';
                v = -v;
            }
            ulltoa((unsigned long long)v, 10, p);
            out_padded(o, buf, width, pad, left_align);
            break;
        }
        case 'u':
        case 'x':
            ulltoa(NEXT_U(ap, longs), *fmt == 'u' ? 10 : 16, buf);
            out_padded(o, buf, width, pad, left_align);
            break;
        case 'p':
            put(o, '0');
            put(o, 'x');
            utoa((unsigned long)(uintptr_t)va_arg(ap, void *), 16, buf);
            out_padded(o, buf, 2 * (int)sizeof(void *), '0', 0);
            break;
        case '%':
            put(o, '%');
            break;
        default:
            put(o, '%');
            put(o, *fmt);
            break;
        }
    }
    flush_out(o);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    struct out o = { 0, { 0 } };
    vformat(&o, fmt, ap);
    va_end(ap);
}

void klog(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    struct out o = { 0, { 0 } };
    vformat(&o, fmt, ap);
    va_end(ap);
}

void boot_title(int on_serial)
{
    (void)on_serial;
}

void boot_step(const char *name)
{
    (void)name;
}

void boot_end(void)
{
}
