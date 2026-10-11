/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * posix_timer.c — timer_create & co.: per-process timers that send a signal
 *
 * What alarm() grew into. A process may own several timers, each with its
 * own clock, its own signal, a value handed back in the signal's siginfo
 * (si_value), and a choice of whom to tell: the whole process
 * (SIGEV_SIGNAL), one of its threads (SIGEV_THREAD_ID), or nobody
 * (SIGEV_NONE: it's only read back with timer_gettime).
 *
 * musl builds SIGEV_THREAD ("call this function") on SIGEV_THREAD_ID: a
 * helper thread waits in sigwaitinfo() for the timer's signal and calls the
 * function when the signal's si_code says SI_TIMER. Vim's 'redrawtime' and
 * regexp timeouts work that way.
 *
 * Every timer is a slot of one fixed table, and its id is the slot number:
 * finding a timer is an index, and the tick never allocates. Armed timers
 * sit on one list sorted by expiry, so the 1 ms tick looks at its head only
 * (as for alarm). Times are whole milliseconds: the timer's resolution.
 */
#include <errno.h>
#include <interrupts.h>
#include <proc.h>
#include <signal.h>
#include <stddef.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>

#define PTIMER_MAX      64
#define PTIMER_PER_PROC 16      /* so one process can't take them all */

/* The longest single wait on the 32-bit millisecond clock (whose times are
   compared as signed differences). Longer timers wait in steps of this. */
#define STEP_MAX (1u << 30)

#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD_ID 4

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME  7
#define TIMER_ABSTIME   1

#define NS_PER_MS  1000000
#define NS_PER_SEC 1000000000

enum { PT_FREE, PT_NEW, PT_READY };     /* PT_NEW: id not yet given out */

struct ptimer {
    struct process *owner;
    uint8_t state, notify, signo, clock;
    uint8_t queued;                     /* its signal is pending, not yet taken */
    struct thread *target;              /* SIGEV_THREAD_ID (NULL once it's gone) */
    user_addr_t value;                  /* sigev_value, back in si_value */
    uint32_t expires;                   /* timer_ms() of the next step (0: disarmed) */
    uint64_t extra_ms;                  /* left to wait after that step */
    uint64_t interval_ms;               /* then again every this long (0: once) */
    int32_t overrun;                    /* expiries while its signal is pending */
    int32_t overrun_last;               /* ...for the last one taken (getoverrun) */
    struct ptimer *next;                /* the armed list */
};

static struct ptimer ptimers[PTIMER_MAX];
static struct ptimer *armed;            /* soonest first */

/* --- the armed list (interrupts off) ---------------------------------------- */

static void unlink_armed(struct ptimer *t)
{
    for (struct ptimer **pp = &armed; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    t->next = NULL;
    t->expires = 0;
}

/* Expire ms after `from`. Sorted insert: O(armed timers), at most PTIMER_MAX. */
static void arm(struct ptimer *t, uint32_t from, uint64_t ms)
{
    uint32_t step = ms > STEP_MAX ? STEP_MAX : (uint32_t)ms;
    t->extra_ms = ms - step;
    t->expires = from + step;
    if (!t->expires)
        t->expires = 1;                 /* 0 means "disarmed" */
    struct ptimer **pp = &armed;
    while (*pp && (int32_t)((*pp)->expires - t->expires) <= 0)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
}

/* Send the timer's signal — unless the last one is still pending: then
   signals don't pile up, the expiry is counted (timer_getoverrun). */
static void fire(struct ptimer *t)
{
    if (t->notify == SIGEV_NONE)
        return;
    struct process *p = t->owner;
    uint64_t bit = SIGBIT(t->signo);
    uint64_t *pending = &p->sig_pending;
    if (t->notify == SIGEV_THREAD_ID) {
        if (!t->target)
            return;                     /* its thread has ended */
        pending = &t->target->sig_pending;
    }
    if (t->queued && (*pending & bit)) {
        if (t->overrun < 0x7FFFFFFF)
            t->overrun++;
        return;
    }
    if (t->notify == SIGEV_THREAD_ID)
        signal_send_thread(t->target, t->signo);
    else
        signal_send(p, t->signo);
    t->queued = (*pending & bit) != 0;  /* (not if the signal is ignored) */
    if (t->queued)
        p->timer_sigs |= bit;
}

void ptimer_tick(uint32_t now)
{
    while (armed && (int32_t)(now - armed->expires) >= 0) {
        struct ptimer *t = armed;
        uint32_t at = t->expires;
        armed = t->next;
        t->next = NULL;
        t->expires = 0;
        if (t->extra_ms) {              /* a long wait: only a step is done */
            arm(t, at, t->extra_ms);
            continue;
        }
        fire(t);
        if (t->interval_ms)
            arm(t, at, t->interval_ms); /* from the expiry: no drift */
    }
}

/* --- delivery ------------------------------------------------------------------ */

int ptimer_siginfo(struct process *p, struct thread *t, int sig, int own,
                   struct sig_origin *o)
{
    uint64_t bit = SIGBIT(sig);
    int found = 0, more = 0;
    for (int i = 0; i < PTIMER_MAX; i++) {
        struct ptimer *pt = &ptimers[i];
        if (pt->owner != p || !pt->queued || pt->signo != sig)
            continue;
        int for_thread = pt->notify == SIGEV_THREAD_ID;
        if (found || for_thread != own || (own && pt->target != t)) {
            more = 1;
            continue;
        }
        pt->queued = 0;
        pt->overrun_last = pt->overrun;
        pt->overrun = 0;
        o->code = SI_TIMER;
        o->timer_id = i;
        o->overrun = pt->overrun_last;
        o->value = pt->value;
        found = 1;
    }
    if (!more)
        p->timer_sigs &= ~bit;
    return found;
}

/* --- lifetime ------------------------------------------------------------------ */

/* A deleted timer's signal that is still pending is taken back, as Linux
   does: unblocking it later must not deliver a signal from a timer that no
   longer exists (for a real-time signal, by default, that ends the program).
   A kill() of the same signal merged into that one bit goes with it. */
static void release(struct ptimer *t)
{
    if (t->expires)
        unlink_armed(t);
    if (t->queued) {
        uint64_t bit = SIGBIT(t->signo);
        if (t->notify == SIGEV_THREAD_ID && t->target)
            t->target->sig_pending &= ~bit;
        else if (t->notify == SIGEV_SIGNAL)
            t->owner->sig_pending &= ~bit;
    }
    t->owner->ntimers--;
    t->owner = NULL;
    t->target = NULL;
    t->state = PT_FREE;
    t->queued = 0;
}

void ptimer_release_all(struct process *p)
{
    uint32_t flags = irq_save();
    for (int i = 0; i < PTIMER_MAX && p->ntimers; i++)
        if (ptimers[i].owner == p)
            release(&ptimers[i]);
    p->timer_sigs = 0;
    irq_restore(flags);
}

/* A thread is leaving: timers aimed at it go quiet (the pointer would
   dangle — thread slots are reused). Interrupts off. */
void ptimer_thread_gone(struct process *p, struct thread *t)
{
    if (!p->ntimers)
        return;
    for (int i = 0; i < PTIMER_MAX; i++)
        if (ptimers[i].owner == p && ptimers[i].target == t)
            ptimers[i].target = NULL;
}

/* The caller's timer with this id, or NULL. Interrupts off. */
static struct ptimer *lookup(uint32_t id)
{
    if (id >= PTIMER_MAX)
        return NULL;
    struct ptimer *t = &ptimers[id];
    return t->owner == proc_current() && t->state == PT_READY ? t : NULL;
}

/* --- the system calls ---------------------------------------------------------- */

/* struct sigevent, the part Linux reads (musl passes only this much: 16 bytes
   on i386, 24 on x86_64, where sigev_value is a pointer-sized union). */
struct ksigevent {
    user_addr_t value;
    int32_t signo, notify, tid;
};

SYSCALL_DEF(sys_timer_create)           /* (clock, sigevent, timer id out) */
{
    struct process *p = proc_current();
    struct ksigevent ev = { 0, SIGALRM, SIGEV_SIGNAL, 0 };
    if (a1 != CLOCK_REALTIME && a1 != CLOCK_MONOTONIC && a1 != CLOCK_BOOTTIME)
        return -EINVAL;
    if (a2 && copy_from_user(&ev, (void *)a2, sizeof ev) < 0)
        return -EFAULT;
    if (ev.notify != SIGEV_SIGNAL && ev.notify != SIGEV_NONE &&
        ev.notify != SIGEV_THREAD_ID)
        return -EINVAL;
    if (ev.notify != SIGEV_NONE && (ev.signo < 1 || ev.signo > NSIG))
        return -EINVAL;

    uint32_t flags = irq_save();
    struct thread *target = NULL;
    if (ev.notify == SIGEV_THREAD_ID) {
        target = thread_by_tid(ev.tid);
        if (!target || target->proc != p) {
            irq_restore(flags);
            return -EINVAL;
        }
    }
    int id = -1;
    if (p->ntimers < PTIMER_PER_PROC)
        for (int i = 0; i < PTIMER_MAX; i++)
            if (ptimers[i].state == PT_FREE) {
                id = i;
                break;
            }
    if (id < 0) {
        irq_restore(flags);
        return -EAGAIN;
    }
    struct ptimer *t = &ptimers[id];
    *t = (struct ptimer){ .owner = p, .state = PT_NEW, .notify = (uint8_t)ev.notify,
                          .signo = (uint8_t)ev.signo, .clock = (uint8_t)a1,
                          .target = target, .value = a2 ? ev.value : (user_addr_t)id };
    p->ntimers++;
    irq_restore(flags);

    /* PT_NEW keeps the slot ours but invisible to the other calls while
       this copy may sleep: no other thread can delete it under us. */
    int32_t uid = id;
    if (copy_to_user((void *)a3, &uid, sizeof uid) < 0) {
        flags = irq_save();
        release(t);
        irq_restore(flags);
        return -EFAULT;
    }
    t->state = PT_READY;
    return 0;
}

/* A timespec -> whole ms, rounded up (a timer never fires early). */
static int ts_to_ms(int64_t sec, int64_t nsec, uint64_t *ms)
{
    if (sec < 0 || nsec < 0 || nsec >= NS_PER_SEC)
        return -EINVAL;
    if (sec > 1000000000000ll)          /* 31,000 years: never, in practice */
        sec = 1000000000000ll;
    *ms = (uint64_t)sec * 1000 + (uint64_t)(nsec + NS_PER_MS - 1) / NS_PER_MS;
    return 0;
}

/* itimerspec in ns: { interval sec, nsec, value sec, nsec }. Interrupts off. */
static void get_value(struct ptimer *t, int64_t v[4])
{
    uint64_t left = 0;
    if (t->expires) {
        /* We are somewhere inside millisecond timer_ms(): up to 1 ms less
           is left than the count of ticks says. Never report 0 for an
           armed timer — 0 means disarmed. */
        left = (uint32_t)(t->expires - timer_ms()) + t->extra_ms;
        left = left > 1 ? (left - 1) * NS_PER_MS : 1;
    }
    uint64_t every = t->interval_ms * NS_PER_MS;
    v[0] = (int64_t)(every / NS_PER_SEC);
    v[1] = (int64_t)(every % NS_PER_SEC);
    v[2] = (int64_t)(left / NS_PER_SEC);
    v[3] = (int64_t)(left % NS_PER_SEC);
}

static int32_t settime(uint32_t id, uint32_t flags, const int64_t v[4], int64_t old[4])
{
    uint64_t every, value;
    if (ts_to_ms(v[0], v[1], &every) < 0 || ts_to_ms(v[2], v[3], &value) < 0)
        return -EINVAL;
    uint32_t irq = irq_save();
    struct ptimer *t = lookup(id);
    if (!t) {
        irq_restore(irq);
        return -EINVAL;
    }
    get_value(t, old);
    if (t->expires)
        unlink_armed(t);
    t->interval_ms = every;
    t->overrun = t->overrun_last = 0;
    if (v[2] || v[3]) {                 /* a value of 0 disarms */
        if (flags & TIMER_ABSTIME) {
            int64_t ns = v[2] * NS_PER_SEC + v[3] - clock_ns(t->clock);
            value = ns > 0 ? ((uint64_t)ns + NS_PER_MS - 1) / NS_PER_MS : 1;
        } else {
            value++;                    /* now is partway through this ms */
        }
        arm(t, timer_ms(), value);
    }
    irq_restore(irq);
    return 0;
}

/* For the old 32-bit itimerspec: seconds past 2^31 become the most it holds. */
static void narrow(const int64_t v[4], int32_t n[4])
{
    for (int i = 0; i < 4; i++)
        n[i] = v[i] > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)v[i];
}

SYSCALL_DEF(sys_timer_settime)          /* (id, flags, new, old): 32-bit timespecs */
{
    int32_t n[4];
    int64_t v[4], o[4];
    if (copy_from_user(n, (void *)a3, sizeof n) < 0)
        return -EFAULT;
    for (int i = 0; i < 4; i++)
        v[i] = n[i];
    int32_t err = settime(a1, a2, v, o);
    if (err)
        return err;
    narrow(o, n);
    return a4 ? copy_to_user((void *)a4, n, sizeof n) : 0;
}

SYSCALL_DEF(sys_timer_settime64)        /* the same with 64-bit timespecs */
{
    int64_t v[4], o[4];
    if (copy_from_user(v, (void *)a3, sizeof v) < 0)
        return -EFAULT;
    int32_t err = settime(a1, a2, v, o);
    if (err)
        return err;
    return a4 ? copy_to_user((void *)a4, o, sizeof o) : 0;
}

static int32_t gettime(uint32_t id, int64_t v[4])
{
    uint32_t irq = irq_save();
    struct ptimer *t = lookup(id);
    if (t)
        get_value(t, v);
    irq_restore(irq);
    return t ? 0 : -EINVAL;
}

SYSCALL_DEF(sys_timer_gettime)
{
    int64_t v[4];
    int32_t n[4];
    int32_t err = gettime(a1, v);
    if (err)
        return err;
    narrow(v, n);
    return copy_to_user((void *)a2, n, sizeof n);
}

SYSCALL_DEF(sys_timer_gettime64)
{
    int64_t v[4];
    int32_t err = gettime(a1, v);
    return err ? err : copy_to_user((void *)a2, v, sizeof v);
}

SYSCALL_DEF(sys_timer_getoverrun)
{
    uint32_t irq = irq_save();
    struct ptimer *t = lookup(a1);
    int32_t n = t ? t->overrun_last : -EINVAL;
    irq_restore(irq);
    return n;
}

SYSCALL_DEF(sys_timer_delete)
{
    uint32_t irq = irq_save();
    struct ptimer *t = lookup(a1);
    if (t)
        release(t);
    irq_restore(irq);
    return t ? 0 : -EINVAL;
}
