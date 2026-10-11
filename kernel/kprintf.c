/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
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
#include <errno.h>
#include <kheap.h>
#include <proc.h>
#include <signal.h>
#include <syscalls.h>
#include <thread.h>
#include <uaccess.h>

void console_write(const char *s, size_t n)
{
    /* Interrupts off for the whole piece: output from two threads (or an
       interrupt) can't get mixed up inside one line. */
    uint32_t flags = irq_save();
    vt_write(s, n);
    serial_write_n(s, n);
    irq_restore(flags);
}

/*
 * The boot's layout. While the kernel starts, the screen shows a title and
 * one line per stage, "Step 07 - Threads" (boot_step), and nothing else
 * unless something goes wrong. The detail (memory map, PCI devices, disks)
 * is printed with klog: serial and the log only, indented four spaces under
 * its step, so the serial port reads as an outline. A kprintf while booting
 * is a warning or an error: indented the same way, and on the screen too.
 * When the first program starts (boot_end), lines go out as they come.
 */
static int booting = 1;                 /* until boot_end() */
static int line_start = 1;              /* the next character begins a line */
static int step_no;                     /* the last "Step NN" printed */

/* Where output goes: kprintf -> screen + serial, klog -> serial only. */
struct out {
    void (*flush)(const char *s, size_t n);
    int indent;                         /* 4 spaces before each line (booting) */
    size_t n;
    char buf[120];
};

static void put(struct out *o, char c)
{
    o->buf[o->n++] = c;
    if (o->n == sizeof o->buf) {
        o->flush(o->buf, o->n);
        o->n = 0;
    }
}

static void out(struct out *o, char c)
{
    if (o->indent && line_start && c != '\n')
        for (int k = 0; k < 4; k++)
            put(o, ' ');
    line_start = c == '\n';
    put(o, c);
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

/* Convert an unsigned number to text in the given base (10 or 16).
   `unsigned long` is the machine word: 32 bits on i386, 64 on x86_64. */
static void utoa(unsigned long v, unsigned base, char *buf)
{
    char tmp[24];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (i)
        *buf++ = tmp[--i];     /* digits came out backwards; reverse them */
    *buf = '\0';
}

/* %ll: always 64 bits (on i386 a slower division from libgcc; rare). */
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

/* The next unsigned argument, sized by the number of 'l's. */
#define NEXT_U(ap, longs) ((longs) >= 2 ? va_arg(ap, unsigned long long) \
                          : (longs) == 1 ? va_arg(ap, unsigned long) \
                          : (unsigned long long)va_arg(ap, unsigned int))

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
        /* 'l' (long) and 'll' (long long), as in %lu. A long is a machine
           word: on i386 32 bits, the same as int, and uint32_t is "unsigned
           long" with this compiler, so %lu is what its format checker wants
           for one. On x86_64 a long is 64 bits. */
        int longs = 0;
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (!*fmt)
            break;          /* format string ended in the middle of a % */

        char buf[24];
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
            long long v = longs >= 2 ? va_arg(ap, long long)
                        : longs == 1 ? va_arg(ap, long) : va_arg(ap, int);
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
            out(o, '0'); out(o, 'x');
            utoa((unsigned long)(uintptr_t)va_arg(ap, void *), 16, buf);
            out_padded(o, buf, 2 * sizeof(void *), '0', 0);
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

/* --- the kernel log (dmesg) ------------------------------------------------ */
/*
 * Everything kprintf and klog print is also kept in a ring buffer, so that
 * `dmesg` (the syslog syscall) can show it later — on a real machine, where
 * nobody was watching the serial port at boot. 64 KiB: a boot is ~4 KiB,
 * but one Wi-Fi firmware crash dump is ~5 KiB, and its debug lines more.
 * When it is full the oldest text goes. `head` counts every byte ever
 * written; the byte for count c is at logbuf[c % LOG_SIZE].
 */
#ifdef MYOS_IWLDEBUG
#define LOG_SIZE 262144                 /* (make IWLDEBUG=1: the driver's debug lines) */
#else
#define LOG_SIZE 65536                  /* a power of two */
#endif
static char logbuf[LOG_SIZE];
static uint32_t log_head;               /* bytes written since boot */
static uint32_t log_cleared;            /* dmesg -c: start of what READ_ALL shows */
static uint32_t log_read;               /* klogd's (READ) position */
static struct wait_queue log_wait;      /* klogd sleeps here for new text */

static void log_append(const char *s, size_t n)
{
    uint32_t flags = irq_save();        /* (kprintf may be called from an IRQ) */
    for (size_t k = 0; k < n; k++)
        logbuf[log_head++ % LOG_SIZE] = s[k];
    irq_restore(flags);
    wq_wake_all(&log_wait);
}

static void to_console(const char *s, size_t n)
{
    log_append(s, n);
    console_write(s, n);
}

static void to_serial(const char *s, size_t n)
{
    log_append(s, n);
    serial_write_n(s, n);
}

static void to_screen(const char *s, size_t n)
{
    log_append(s, n);
    uint32_t flags = irq_save();
    vt_write(s, n);
    irq_restore(flags);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct out o = { to_console, booting, 0, {0} };
    vformat(&o, fmt, ap);
    va_end(ap);
}

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef TRACE_EXEC
    struct out o = { to_console, booting, 0, {0} };   /* make TRACE=1: on the screen too */
#else
    struct out o = { to_serial, booting, 0, {0} };
#endif
    vformat(&o, fmt, ap);
    va_end(ap);
}

/* One whole line, never indented: the boot's own. A line someone left
   open (a kprintf without '\n') is ended first. */
static void boot_line(void (*flush)(const char *, size_t), const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct out o = { flush, 0, 0, {0} };
    if (!line_start)
        put(&o, '\n');
    vformat(&o, fmt, ap);
    va_end(ap);
}

void boot_title(int on_serial)
{
    void (*flush)(const char *, size_t) = on_serial ? to_console : to_screen;
    vt_set_color(VT_LIGHT_GREEN, VT_BLACK);
    boot_line(flush, "MyOS, written by Rodger Wilson...\n");
    vt_set_color(VT_LIGHT_GREY, VT_BLACK);
    boot_line(flush, "Step %02u - %s\n", (unsigned)++step_no, "Boot loader");
}

void boot_step(const char *name)
{
    boot_line(to_console, "Step %02u - %s\n", (unsigned)++step_no, name);
}

void boot_end(void)
{
    booting = 0;
}

/* Copy log bytes [from, to) (counts, at most LOG_SIZE apart) out: snapshot
   them with interrupts off, then copy to the program (which may sleep). */
static int32_t log_copy_out(uintptr_t user, uint32_t from, uint32_t to)
{
    uint32_t n = to - from;
    char *tmp = kmalloc(n ? n : 1);
    if (!tmp)
        return -ENOMEM;
    uint32_t flags = irq_save();
    if (log_head - from > LOG_SIZE)     /* overwritten meanwhile: skip on */
        from = log_head - LOG_SIZE, n = to > from ? to - from : 0;
    for (uint32_t k = 0; k < n; k++)
        tmp[k] = logbuf[(from + k) % LOG_SIZE];
    irq_restore(flags);
    int err = copy_to_user((void *)user, tmp, n);
    kfree(tmp);
    return err < 0 ? err : (int32_t)n;
}

#define SYSLOG_ACTION_CLOSE         0
#define SYSLOG_ACTION_OPEN          1
#define SYSLOG_ACTION_READ          2
#define SYSLOG_ACTION_READ_ALL      3
#define SYSLOG_ACTION_READ_CLEAR    4
#define SYSLOG_ACTION_CLEAR         5
#define SYSLOG_ACTION_CONSOLE_OFF   6
#define SYSLOG_ACTION_CONSOLE_ON    7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD   9
#define SYSLOG_ACTION_SIZE_BUFFER   10

/*
 * syslog(type, buf, len) — glibc and musl call it klogctl. Reading the whole
 * log is for everyone (Linux's default, dmesg_restrict = 0); consuming or
 * clearing it is root's. The console levels are accepted and change nothing:
 * kernel messages here have no levels to filter on.
 */
SYSCALL_DEF(sys_syslog)
{
    int type = (int)a1;
    int32_t len = (int32_t)a3;
    if (type != SYSLOG_ACTION_READ_ALL && type != SYSLOG_ACTION_SIZE_BUFFER &&
        proc_current()->cred.euid != 0)
        return -EPERM;
    switch (type) {
    case SYSLOG_ACTION_CLOSE:
    case SYSLOG_ACTION_OPEN:
    case SYSLOG_ACTION_CONSOLE_OFF:
    case SYSLOG_ACTION_CONSOLE_ON:
        return 0;
    case SYSLOG_ACTION_CONSOLE_LEVEL:
        return len < 1 || len > 8 ? -EINVAL : 0;
    case SYSLOG_ACTION_SIZE_BUFFER:
        return LOG_SIZE;
    case SYSLOG_ACTION_SIZE_UNREAD: {
        uint32_t n = log_head - log_read;
        return n > LOG_SIZE ? LOG_SIZE : (int32_t)n;
    }
    case SYSLOG_ACTION_CLEAR:
        log_cleared = log_head;
        return 0;
    case SYSLOG_ACTION_READ: {          /* klogd: wait for new text, take it */
        if (!a2 || len < 0)
            return -EINVAL;
        while (log_read == log_head) {
            if (signal_pending())
                return -EINTR;
            wq_sleep(&log_wait);
        }
        if (log_head - log_read > LOG_SIZE)
            log_read = log_head - LOG_SIZE;
        uint32_t to = log_head - log_read > (uint32_t)len ? log_read + len : log_head;
        int32_t n = log_copy_out(a2, log_read, to);
        if (n > 0)
            log_read += n;
        return n;
    }
    case SYSLOG_ACTION_READ_ALL:
    case SYSLOG_ACTION_READ_CLEAR: {    /* dmesg: the newest len bytes */
        if (!a2 || len < 0)
            return -EINVAL;
        uint32_t from = log_cleared, to = log_head;
        if (to - from > LOG_SIZE)
            from = to - LOG_SIZE;
        if (to - from > (uint32_t)len)
            from = to - len;
        int32_t n = log_copy_out(a2, from, to);
        if (n >= 0 && type == SYSLOG_ACTION_READ_CLEAR)
            log_cleared = to;
        return n;
    }
    default:
        return -EINVAL;
    }
}
