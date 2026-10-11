/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * futex.c — "fast user-space mutex": the one system call under every
 * pthread mutex, condition variable, semaphore and pthread_join (musl), and
 * under GLib's GMutex and GCond.
 *
 * The idea: a lock is just an int in the program's memory. Taking a free
 * lock is one atomic instruction in user space, with no system call at
 * all. Only when a thread has to WAIT does it ask the kernel:
 *
 *   futex(&word, FUTEX_WAIT, expected)  sleep, but only if word still holds
 *                                       `expected` (checked here, so a wake
 *                                       that came just before can't be lost)
 *   futex(&word, FUTEX_WAKE, n)         wake up to n threads sleeping on it
 *
 * The kernel keeps no state about the lock itself, only who sleeps on
 * which address. Sleepers are kept in a small hash table keyed by (address
 * space, address), so WAIT and WAKE cost O(sleepers in one bucket), not a
 * search of all threads.
 *
 * Interrupts are off for the whole system call (as for every syscall here),
 * so between reading the word and going to sleep no other thread can run
 * and change it: the check-and-sleep is atomic on our one CPU.
 */
#include <errno.h>
#include <futex.h>
#include <proc.h>
#include <signal.h>
#include <stddef.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_PRIVATE_FLAG  128     /* only threads of one process share it */
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK      ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define FUTEX_BUCKETS 64            /* power of two */
static struct wait_queue buckets[FUTEX_BUCKETS];

static struct wait_queue *bucket(struct mm *mm, uintptr_t addr)
{
    uint32_t h = (uint32_t)((addr >> 2) ^ ((uintptr_t)mm >> 6));
    h ^= h >> 11;
    return &buckets[h & (FUTEX_BUCKETS - 1)];
}

static int is_waiter(const struct thread *t, struct mm *mm, uintptr_t addr)
{
    return t->futex_mm == mm && t->futex_addr == addr;
}

int futex_wake(struct mm *mm, uintptr_t uaddr, int n)
{
    struct wait_queue *q = bucket(mm, uaddr);
    int woken = 0;
    /* A queue is newest-first. Waking one thread (a mutex unlock: the
       common case) takes the OLDEST waiter, so nobody waits forever while
       newer arrivals keep getting the lock. */
    while (woken < n) {
        struct thread *pick = NULL;
        for (struct thread *t = q->first; t; t = t->wq_next)
            if (is_waiter(t, mm, uaddr))
                pick = t;               /* the last match is the oldest */
        if (!pick)
            break;
        pick->futex_woken = 1;
        pick->futex_mm = NULL;
        thread_wake(pick);              /* also takes it off the queue */
        woken++;
    }
    return woken;
}

/* Move up to n sleepers from uaddr to uaddr2 without waking them
   (pthread_cond_broadcast: waking them all just to have them pile up on the
   mutex would be wasted work). */
static int requeue(struct mm *mm, uintptr_t uaddr, uintptr_t uaddr2, int n)
{
    struct wait_queue *from = bucket(mm, uaddr), *to = bucket(mm, uaddr2);
    int moved = 0;
    struct thread **pp = &from->first;
    while (*pp && moved < n) {
        struct thread *t = *pp;
        if (!is_waiter(t, mm, uaddr)) {
            pp = &t->wq_next;
            continue;
        }
        t->futex_addr = uaddr2;
        moved++;
        if (from == to) {
            pp = &t->wq_next;
            continue;
        }
        *pp = t->wq_next;               /* out of the old bucket... */
        t->wq = to;                     /* ...into the new one */
        t->wq_next = to->first;
        to->first = t;
    }
    return moved;
}

/* A relative timeout -> the timer_ms() value to wake at (rounded up, so
   the sleep is never shorter than asked). 0 in *deadline: no timeout. */
static int read_timeout(uintptr_t utime, int time64, uint32_t *deadline, int *zero)
{
    int64_t sec, nsec;
    if (time64) {
        int64_t ts[2];
        if (copy_from_user(ts, (void *)utime, sizeof ts) < 0)
            return -EFAULT;
        sec = ts[0];
        nsec = ts[1];
    } else {
        int32_t ts[2];
        if (copy_from_user(ts, (void *)utime, sizeof ts) < 0)
            return -EFAULT;
        sec = ts[0];
        nsec = ts[1];
    }
    if (sec < 0 || nsec < 0 || nsec >= 1000000000)
        return -EINVAL;
    *zero = sec == 0 && nsec == 0;
    if (sec > 0x7FFFFFFF / 1000 - 1)    /* longer than ~24 days: no limit */
        return 0;
    uint32_t ms = (uint32_t)sec * 1000 + (uint32_t)((nsec + 999999) / 1000000);
    *deadline = timer_ms() + ms + 1;    /* +1: the current ms is partly gone */
    if (*deadline == 0)
        *deadline = 1;                  /* 0 means "no deadline" */
    return 0;
}

static int32_t futex_wait(struct mm *mm, uintptr_t uaddr, uint32_t val,
                          uintptr_t utime, int time64)
{
    uint32_t deadline = 0;
    int zero = 0;
    if (utime) {
        int err = read_timeout(utime, time64, &deadline, &zero);
        if (err)
            return err;
    }
    uint32_t now;
    if (get_user32(&now, uaddr) < 0)
        return -EFAULT;
    if (now != val)
        return -EAGAIN;                 /* changed already: don't sleep */
    if (zero)
        return -ETIMEDOUT;
    if (signal_pending())
        return -EINTR;

    struct thread *t = thread_current();
    t->futex_mm = mm;
    t->futex_addr = uaddr;
    t->futex_woken = 0;
    int timed_out = wq_sleep_until(bucket(mm, uaddr), deadline);
    t->futex_mm = NULL;
    if (t->futex_woken)
        return 0;
    if (timed_out)
        return -ETIMEDOUT;
    return -EINTR;                      /* a signal (or exit_group) */
}

int32_t sys_futex_impl(uintptr_t uaddr, uint32_t op, uint32_t val, uintptr_t timeout,
                       uintptr_t uaddr2, uint32_t val3, int time64)
{
    if (uaddr & 3)
        return -EINVAL;
    /* Shared futexes (no PRIVATE flag) can also link processes through
       shared memory; Linux then keys them by the physical page. Here every
       futex is keyed by the address space, which is right for all threads
       of one process (docs/gaps.md). */
    struct mm *mm = proc_current()->mm;
    switch (op & FUTEX_CMD_MASK) {
    case FUTEX_WAIT:
        return futex_wait(mm, uaddr, val, timeout, time64);
    case FUTEX_WAKE:
        return futex_wake(mm, uaddr, (int32_t)val < 0 ? 0x7FFFFFFF : (int)val);
    case FUTEX_CMP_REQUEUE: {
        uint32_t now;
        if (get_user32(&now, uaddr) < 0)
            return -EFAULT;
        if (now != val3)
            return -EAGAIN;
    }   /* fall through */
    case FUTEX_REQUEUE: {
        if (uaddr2 & 3)
            return -EINVAL;
        int n = futex_wake(mm, uaddr, (int32_t)val < 0 ? 0x7FFFFFFF : (int)val);
        /* (The 4th argument is the requeue count here, not a timeout.) */
        int32_t nreq = (int32_t)timeout < 0 ? 0x7FFFFFFF : (int32_t)timeout;
        return n + requeue(mm, uaddr, uaddr2, nreq);
    }
    default:
        return -ENOSYS;
    }
}
