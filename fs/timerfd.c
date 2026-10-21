/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * timerfd.c — timerfd_create & co.: a timer you read, or poll, as a file
 *
 * Reading it returns, as 8 bytes, how many times it has expired since the
 * last read; it blocks until that is at least 1. libwayland's event loop
 * runs every timer this way (wl_event_loop_add_timer), so a Wayland
 * window server needs it; glib and systemd-style loops use it too.
 *
 * The expiry is a timer_list (kernel/timer_list.c): no work on the tick,
 * and the function only counts, re-arms a repeating timer from its own
 * expiry (no drift) and wakes readers. Times are whole milliseconds, the
 * jiffy; a timer never fires early.
 */
#include <errno.h>
#include <interrupts.h>
#include <kheap.h>
#include <linux/timer.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <uaccess.h>
#include <vfs.h>

#define TFD_NONBLOCK       O_NONBLOCK
#define TFD_CLOEXEC        O_CLOEXEC
#define TFD_TIMER_ABSTIME  1
#define TFD_TIMER_CANCEL_ON_SET 2       /* accepted; the clock is never set under us */

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME  7

#define NS_PER_MS  1000000
#define NS_PER_SEC 1000000000

struct timerfd {
    struct timer_list timer;
    uint64_t ticks;                     /* expiries not yet read */
    uint64_t interval;                  /* ms; 0: once */
    uint32_t clock;
    struct wait_queue wait;
};

static struct inode tfd_inode = {
    .ino = 0, .mode = S_IFCHR | 0600, .nlink = 1, .name = "timerfd", .name_len = 7,
};

/* In ktimers, interrupts off: count, re-arm, wake. */
static void tfd_expired(struct timer_list *t)
{
    struct timerfd *tf = from_timer(tf, t, timer);
    tf->ticks++;
    if (tf->interval)
        mod_timer(&tf->timer, tf->timer.expires + (unsigned long)tf->interval);
    wq_wake_all(&tf->wait);
    wq_wake_all(&poll_wait);
}

static int32_t tfd_read(struct file *f, void *user_buf, uint32_t n)
{
    struct timerfd *tf = f->priv;
    if (n < 8)
        return -EINVAL;
    file_get(f);
    uint32_t irq = irq_save();
    while (tf->ticks == 0) {
        irq_restore(irq);
        if (f->flags & O_NONBLOCK) {
            file_put(f);
            return -EAGAIN;
        }
        if (signal_pending()) {
            file_put(f);
            return -ERESTARTSYS;
        }
        wq_sleep(&tf->wait);
        irq = irq_save();
    }
    uint64_t v = tf->ticks;
    tf->ticks = 0;
    irq_restore(irq);
    file_put(f);
    return copy_to_user(user_buf, &v, 8) < 0 ? -EFAULT : 8;
}

static uint32_t tfd_poll(struct file *f)
{
    struct timerfd *tf = f->priv;
    return tf->ticks ? 0x001 : 0;       /* POLLIN */
}

static void tfd_release(struct file *f)
{
    struct timerfd *tf = f->priv;
    timer_delete(&tf->timer);
    kfree(tf);
}

static const struct file_ops tfd_ops = {
    .read = tfd_read, .poll = tfd_poll, .release = tfd_release,
};

SYSCALL_DEF(sys_timerfd_create)         /* (clock, flags) */
{
    if (a1 != CLOCK_REALTIME && a1 != CLOCK_MONOTONIC && a1 != CLOCK_BOOTTIME)
        return -EINVAL;
    if (a2 & ~(TFD_NONBLOCK | TFD_CLOEXEC))
        return -EINVAL;
    struct timerfd *tf = kmalloc(sizeof *tf);
    if (!tf)
        return -ENOMEM;
    memset(tf, 0, sizeof *tf);
    timer_setup(&tf->timer, tfd_expired, 0);
    tf->clock = a1;
    struct file *f = file_new(&tfd_ops, &tfd_inode, O_RDONLY | (a2 & O_NONBLOCK));
    if (!f) {
        kfree(tf);
        return -ENOMEM;
    }
    f->priv = tf;
    int fd = fd_install(f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (a2 & TFD_CLOEXEC)
        fd_set_cloexec(proc_current(), fd, 1);
    return fd;
}

static struct timerfd *lookup(int fd, int32_t *err)
{
    struct file *f = fd_get(fd);
    if (!f) {
        *err = -EBADF;
        return NULL;
    }
    if (f->ops != &tfd_ops) {
        *err = -EINVAL;
        return NULL;
    }
    return f->priv;
}

/* A timespec -> whole ms, rounded up. */
static int ts_to_ms(int64_t sec, int64_t nsec, uint64_t *ms)
{
    if (sec < 0 || nsec < 0 || nsec >= NS_PER_SEC)
        return -EINVAL;
    if (sec > 1000000000ll)             /* 31 years: longer than a jiffy can count */
        sec = 1000000000ll;
    *ms = (uint64_t)sec * 1000 + (uint64_t)(nsec + NS_PER_MS - 1) / NS_PER_MS;
    return 0;
}

/* itimerspec in ns: { interval sec, nsec, value sec, nsec }. Interrupts off. */
static void get_value(struct timerfd *tf, int64_t v[4])
{
    uint64_t left = 0;
    if (timer_pending(&tf->timer)) {
        long d = (long)(tf->timer.expires - jiffies);
        left = d > 0 ? (uint64_t)d * NS_PER_MS : 1;
    }
    uint64_t every = tf->interval * NS_PER_MS;
    v[0] = (int64_t)(every / NS_PER_SEC);
    v[1] = (int64_t)(every % NS_PER_SEC);
    v[2] = (int64_t)(left / NS_PER_SEC);
    v[3] = (int64_t)(left % NS_PER_SEC);
}

static int32_t settime(int fd, uint32_t flags, const int64_t v[4], int64_t old[4])
{
    int32_t err = -EBADF;
    uint64_t every, value;
    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
        return -EINVAL;
    if (ts_to_ms(v[0], v[1], &every) < 0 || ts_to_ms(v[2], v[3], &value) < 0)
        return -EINVAL;
    struct timerfd *tf = lookup(fd, &err);
    if (!tf)
        return err;
    uint32_t irq = irq_save();
    get_value(tf, old);
    timer_delete(&tf->timer);
    tf->ticks = 0;
    tf->interval = every;
    if (v[2] || v[3]) {                 /* a value of 0 disarms */
        if (flags & TFD_TIMER_ABSTIME) {
            int64_t ns = v[2] * NS_PER_SEC + v[3] - clock_ns(tf->clock);
            value = ns > 0 ? ((uint64_t)ns + NS_PER_MS - 1) / NS_PER_MS : 0;
        }
        mod_timer(&tf->timer, jiffies + (unsigned long)value + 1);  /* now is partway through a jiffy */
    }
    irq_restore(irq);
    return 0;
}

static int32_t gettime(int fd, int64_t v[4])
{
    int32_t err = -EBADF;
    struct timerfd *tf = lookup(fd, &err);
    if (!tf)
        return err;
    uint32_t irq = irq_save();
    get_value(tf, v);
    irq_restore(irq);
    return 0;
}

/* The old 32-bit itimerspec: seconds past 2^31 become the most it holds. */
static void narrow(const int64_t v[4], int32_t n[4])
{
    for (int i = 0; i < 4; i++)
        n[i] = v[i] > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)v[i];
}

SYSCALL_DEF(sys_timerfd_settime)        /* (fd, flags, new, old): 32-bit timespecs */
{
    int32_t n[4];
    int64_t v[4], o[4];
    if (copy_from_user(n, (void *)a3, sizeof n) < 0)
        return -EFAULT;
    for (int i = 0; i < 4; i++)
        v[i] = n[i];
    int32_t err = settime((int)a1, a2, v, o);
    if (err)
        return err;
    narrow(o, n);
    return a4 ? copy_to_user((void *)a4, n, sizeof n) : 0;
}

SYSCALL_DEF(sys_timerfd_settime64)      /* the same with 64-bit timespecs */
{
    int64_t v[4], o[4];
    if (copy_from_user(v, (void *)a3, sizeof v) < 0)
        return -EFAULT;
    int32_t err = settime((int)a1, a2, v, o);
    if (err)
        return err;
    return a4 ? copy_to_user((void *)a4, o, sizeof o) : 0;
}

SYSCALL_DEF(sys_timerfd_gettime)
{
    int64_t v[4];
    int32_t n[4];
    int32_t err = gettime((int)a1, v);
    if (err)
        return err;
    narrow(v, n);
    return copy_to_user((void *)a2, n, sizeof n);
}

SYSCALL_DEF(sys_timerfd_gettime64)
{
    int64_t v[4];
    int32_t err = gettime((int)a1, v);
    return err ? err : copy_to_user((void *)a2, v, sizeof v);
}
