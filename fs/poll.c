/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * poll.c — poll() and select(): wait for any of several files
 *
 * "Tell me when the keyboard or this pipe has data — or after 100 ms."
 * Each file says whether it's ready (file_ops.poll; files that are always
 * ready have none). If nothing is ready, we sleep on one shared queue that
 * the terminal and pipes wake whenever their state changes, and look again.
 */
#include <errno.h>
#include <kheap.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <user.h>
#include <vfs.h>
#ifdef BENCH
#include <bench.h>
#endif

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct wait_queue poll_wait;            /* woken by tty.c and pipe.c */

struct pollfd { int32_t fd; int16_t events, revents; };

static uint32_t file_ready(int fd)
{
    struct file *f = fd_get(fd);
    if (!f)
        return POLLNVAL;
    return f->ops->poll ? f->ops->poll(f) : (POLLIN | POLLOUT);
}

/* Fill in revents; return how many are ready. */
static int scan(struct pollfd *p, uint32_t n)
{
    int ready = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i].fd < 0) {
            p[i].revents = 0;
            continue;
        }
        uint32_t r = file_ready(p[i].fd);
        p[i].revents = (int16_t)(r & (p[i].events | POLLERR | POLLHUP | POLLNVAL));
        if (p[i].revents)
            ready++;
    }
    return ready;
}

/* The heart of both: timeout_ms < 0 means "no limit". */
static int32_t do_poll(struct pollfd *p, uint32_t n, int32_t timeout_ms)
{
    uint32_t deadline = timeout_ms > 0 ? timer_ms() + (uint32_t)timeout_ms + 1 : 0;
    for (;;) {
        int ready = scan(p, n);
        if (ready || timeout_ms == 0)
            return ready;
        if (signal_pending())
            return -EINTR;
#ifdef BENCH
        bench_input_wait();
#endif
        if (wq_sleep_until(&poll_wait, deadline))
            return scan(p, n);          /* time's up: one last look */
    }
}

#define POLL_MAX 64

static int32_t poll_user(uintptr_t user_fds, uint32_t n, int32_t timeout_ms)
{
    struct pollfd p[POLL_MAX];
    if (n > POLL_MAX)
        return -EINVAL;
    if (copy_from_user(p, (void *)user_fds, n * sizeof p[0]) < 0)
        return -EFAULT;
    int32_t r = do_poll(p, n, timeout_ms);
    if (r >= 0 && copy_to_user((void *)user_fds, p, n * sizeof p[0]) < 0)
        return -EFAULT;
    return r;
}

SYSCALL_DEF(sys_poll)                   /* (fds, nfds, timeout_ms) */
{
    return poll_user(a1, a2, (int32_t)a3);
}

/* timespec64 -> ms (-1 for NULL: no limit) */
static int32_t ts64_ms(uintptr_t user_ts, int *err)
{
    if (!user_ts)
        return -1;
    int64_t ts[2];
    if (copy_from_user(ts, (void *)user_ts, sizeof ts) < 0) {
        *err = -EFAULT;
        return 0;
    }
    int64_t ms = ts[0] * 1000 + (ts[1] + 999999) / 1000000;
    return ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)ms;
}

/* timespec32 -> ms (-1 for NULL: no limit) */
static int32_t ts32_ms(uintptr_t user_ts, int *err)
{
    if (!user_ts)
        return -1;
    int32_t ts[2];
    if (copy_from_user(ts, (void *)user_ts, sizeof ts) < 0) {
        *err = -EFAULT;
        return 0;
    }
    return ts[0] * 1000 + (ts[1] + 999999) / 1000000;
}

SYSCALL_DEF(sys_ppoll32)                /* (fds, nfds, timespec32 *, sigmask *, size) */
{
    int err = 0;
    int32_t ms = ts32_ms(a3, &err);
    return err ? err : poll_user(a1, a2, ms);
}

SYSCALL_DEF(sys_ppoll64)                /* (fds, nfds, timespec64 *, sigmask *, size) */
{
    int err = 0;
    int32_t ms = ts64_ms(a3, &err);
    return err ? err : poll_user(a1, a2, ms);
}

/* select(): fd_set bitmaps instead of a list. Converted to poll. The sets
   are arrays of longs (a program's long: 4 or 8 bytes), nfds bits used,
   read and written in whole longs as Linux does. Up to PROC_FILES fds
   (FD_SETSIZE, 1024). */
static int32_t do_select(uint32_t nfds, uintptr_t rd, uintptr_t wr, uintptr_t ex, int32_t ms)
{
    if (nfds > PROC_FILES)
        nfds = PROC_FILES;
    uint32_t lbits = 8 * sizeof(user_addr_t);   /* a program's long */
    uint32_t bytes = (nfds + lbits - 1) / lbits * (lbits / 8);
    uint32_t in[3][PROC_FILES / 32], out[3][PROC_FILES / 32];
    uintptr_t user[3] = { rd, wr, ex };
    memset(in, 0, sizeof in);
    memset(out, 0, sizeof out);
    for (int k = 0; k < 3; k++)
        if (user[k] && bytes && copy_from_user(in[k], (const void *)user[k], bytes) < 0)
            return -EFAULT;

    /* Few fds (the usual case): the list on the stack; else the heap. */
    struct pollfd small[32], *p = small;
    uint32_t want = 0;
    for (uint32_t fd = 0; fd < nfds; fd++)
        want += ((in[0][fd / 32] | in[1][fd / 32] | in[2][fd / 32]) >> (fd % 32)) & 1;
    if (want > 32 && !(p = kmalloc(want * sizeof *p)))
        return -ENOMEM;
    uint32_t n = 0;
    for (uint32_t fd = 0; fd < nfds; fd++) {
        uint32_t w = fd / 32, b = 1u << (fd % 32);
        int16_t ev = (in[0][w] & b ? POLLIN : 0) | (in[1][w] & b ? POLLOUT : 0);
        if (ev || (in[2][w] & b))
            p[n++] = (struct pollfd){ (int32_t)fd, ev, 0 };
    }
    int32_t r = do_poll(p, n, ms);
    int count = 0;
    for (uint32_t i = 0; r >= 0 && i < n; i++) {
        uint32_t w = (uint32_t)p[i].fd / 32, b = 1u << (p[i].fd % 32);
        if ((p[i].revents & (POLLIN | POLLHUP | POLLERR)) && (in[0][w] & b))
            out[0][w] |= b, count++;
        if ((p[i].revents & (POLLOUT | POLLERR)) && (in[1][w] & b))
            out[1][w] |= b, count++;
        if (p[i].revents & POLLNVAL)
            r = -EBADF;
    }
    if (p != small)
        kfree(p);
    if (r < 0)
        return r;
    for (int k = 0; k < 3; k++)
        if (user[k] && bytes && copy_to_user((void *)user[k], out[k], bytes) < 0)
            return -EFAULT;
    return count;
}

SYSCALL_DEF(sys_select)                 /* (nfds, rd, wr, ex, struct timeval *) */
{
    int32_t ms = -1;
    if (a5) {
        long tv[2];                     /* two longs: 32 bits on i386, 64 on x86_64 */
        if (copy_from_user(tv, (void *)a5, sizeof tv) < 0)
            return -EFAULT;
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000)
            return -EINVAL;
        if (tv[0] > 0x7FFFFFFF / 1000 - 1)
            tv[0] = 0x7FFFFFFF / 1000 - 1;
        ms = (int32_t)(tv[0] * 1000 + (tv[1] + 999) / 1000);
    }
    return do_select(a1, a2, a3, a4, ms);
}

SYSCALL_DEF(sys_pselect64)              /* (nfds, rd, wr, ex, timespec64 *, sigmask) */
{
    int err = 0;
    int32_t ms = ts64_ms(a5, &err);
    return err ? err : do_select(a1, a2, a3, a4, ms);
}

SYSCALL_DEF(sys_pselect32)              /* (nfds, rd, wr, ex, timespec32 *, sigmask) */
{
    int err = 0;
    int32_t ms = ts32_ms(a5, &err);
    return err ? err : do_select(a1, a2, a3, a4, ms);
}
