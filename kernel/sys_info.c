/*
 * sys_info.c — "who am I, what time is it" system calls, and thread-local
 * storage: the things every program asks the kernel when it starts.
 */
#include <errno.h>
#include <gdt.h>
#include <kprintf.h>
#include <mm.h>
#include <pmm.h>
#include <proc.h>
#include <signal.h>
#include <rtc.h>
#include <string.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>
#include <cycles.h>
#include <uaccess.h>
#include <vfs.h>

/* --- the system's name ------------------------------------------------------ */

/* struct utsname: six fields of 65 bytes. We report "Linux" as the system
   name, because programs (and C libraries) check it to decide what the
   kernel can do — and MyOS speaks Linux's system call language. The
   release string says what it really is. */
SYSCALL_DEF(sys_uname)
{
    static const char fields[6][65] = {
        "Linux", "myos", "6.1.0-MyOS", "#1 MyOS", "i686", "(none)",
    };
    return copy_to_user((void *)a1, fields, sizeof fields);
}

/* --- users and groups: everything runs as root (uid 0) -------------------------- */

SYSCALL_DEF(sys_getid)    { return 0; }
SYSCALL_DEF(sys_setid)    { return a1 == 0 || a1 == 0xFFFFFFFF ? 0 : -EPERM; }
SYSCALL_DEF(sys_getgroups) { return 0; }        /* no supplementary groups */

/* getresuid(&r, &e, &s) / getresgid: all three are 0. */
SYSCALL_DEF(sys_getresid)
{
    uint32_t zero = 0;
    if (copy_to_user((void *)a1, &zero, 4) < 0 || copy_to_user((void *)a2, &zero, 4) < 0 ||
        copy_to_user((void *)a3, &zero, 4) < 0)
        return -EFAULT;
    return 0;
}

/* --- time ------------------------------------------------------------------------ */

/* The wall clock: the CMOS clock's time at boot, plus the 1 ms timer.
   (Millisecond resolution for now; a vDSO with nanoseconds is step 11.) */
static uint32_t boot_epoch;

void time_init(void)
{
    boot_epoch = rtc_read_epoch();
}

uint32_t time_now(void)
{
    return boot_epoch + timer_ms() / 1000;
}

#define CLOCK_REALTIME          0
#define CLOCK_REALTIME_COARSE   5
#define CLOCK_REALTIME_ALARM    8
#define TIMER_ABSTIME           1

/* Nanoseconds on clock clk: realtime (since 1970) or monotonic (boot). */
static int64_t clock_ns(uint32_t clk)
{
    int64_t up = (int64_t)timer_ms() * 1000000;
    if (clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE || clk == CLOCK_REALTIME_ALARM)
        return (int64_t)boot_epoch * 1000000000 + up;
    return up;                          /* monotonic, boottime, cputime... */
}

SYSCALL_DEF(sys_time)
{
    int32_t t = (int32_t)(clock_ns(CLOCK_REALTIME) / 1000000000);
    if (a1 && copy_to_user((void *)a1, &t, 4) < 0)
        return -EFAULT;
    return t;
}

SYSCALL_DEF(sys_gettimeofday)
{
    int64_t ns = clock_ns(CLOCK_REALTIME);
    int32_t tv[2] = { (int32_t)(ns / 1000000000), (int32_t)(ns % 1000000000 / 1000) };
    if (a1 && copy_to_user((void *)a1, tv, sizeof tv) < 0)
        return -EFAULT;
    if (a2) {
        int32_t tz[2] = { 0, 0 };       /* UTC */
        if (copy_to_user((void *)a2, tz, sizeof tz) < 0)
            return -EFAULT;
    }
    return 0;
}

/* clock_gettime with the old 32-bit struct timespec { int sec; int nsec; } */
SYSCALL_DEF(sys_clock_gettime)
{
    int64_t ns = clock_ns(a1);
    int32_t ts[2] = { (int32_t)(ns / 1000000000), (int32_t)(ns % 1000000000) };
    return copy_to_user((void *)a2, ts, sizeof ts);
}

/* ...and with the 64-bit one (musl uses this first): { int64 sec; int64 nsec; } */
SYSCALL_DEF(sys_clock_gettime64)
{
    int64_t ns = clock_ns(a1);
    int64_t ts[2] = { ns / 1000000000, ns % 1000000000 };
    return copy_to_user((void *)a2, ts, sizeof ts);
}

SYSCALL_DEF(sys_clock_getres)
{
    int32_t ts[2] = { 0, 1000000 };     /* 1 ms */
    return a2 ? copy_to_user((void *)a2, ts, sizeof ts) : 0;
}

SYSCALL_DEF(sys_clock_getres64)
{
    int64_t ts[2] = { 0, 1000000 };
    return a2 ? copy_to_user((void *)a2, ts, sizeof ts) : 0;
}

/* Sleep for ns nanoseconds (rounded up to whole ms). 0, or -EINTR with
   the time left in *left_ns if a signal came first. */
static int sleep_ns(int64_t ns, int64_t *left_ns)
{
    if (ns <= 0)
        return 0;
    uint32_t start = timer_ms();
    uint32_t deadline = start + (uint32_t)((ns + 999999) / 1000000) + 1;
    struct wait_queue q = { 0 };        /* only the timer (or a signal) wakes us */
    while ((int32_t)(timer_ms() - deadline) < 0) {
        if (signal_pending()) {
            *left_ns = (int64_t)(deadline - timer_ms()) * 1000000;
            return -EINTR;
        }
        wq_sleep_until(&q, deadline);
    }
    return 0;
}

static int32_t nanosleep_common(int64_t ns, uint32_t user_rem, int wide)
{
    int64_t left = 0;
    int err = sleep_ns(ns, &left);
    if (err && user_rem) {
        if (wide) {
            int64_t ts[2] = { left / 1000000000, left % 1000000000 };
            copy_to_user((void *)user_rem, ts, sizeof ts);
        } else {
            int32_t ts[2] = { (int32_t)(left / 1000000000), (int32_t)(left % 1000000000) };
            copy_to_user((void *)user_rem, ts, sizeof ts);
        }
    }
    return err;
}

SYSCALL_DEF(sys_nanosleep)
{
    int32_t ts[2];
    if (copy_from_user(ts, (void *)a1, sizeof ts) < 0)
        return -EFAULT;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000)
        return -EINVAL;
    return nanosleep_common((int64_t)ts[0] * 1000000000 + ts[1], a2, 0);
}

SYSCALL_DEF(sys_clock_nanosleep)        /* (clock, flags, req32, rem32) */
{
    int32_t ts[2];
    if (copy_from_user(ts, (void *)a3, sizeof ts) < 0)
        return -EFAULT;
    int64_t ns = (int64_t)ts[0] * 1000000000 + ts[1];
    if (a2 & TIMER_ABSTIME)
        ns -= clock_ns(a1);
    return nanosleep_common(ns, (a2 & TIMER_ABSTIME) ? 0 : a4, 0);
}

SYSCALL_DEF(sys_clock_nanosleep64)      /* (clock, flags, req64, rem64) */
{
    int64_t ts[2];
    if (copy_from_user(ts, (void *)a3, sizeof ts) < 0)
        return -EFAULT;
    int64_t ns = ts[0] * 1000000000 + ts[1];
    if (a2 & TIMER_ABSTIME)
        ns -= clock_ns(a1);
    return nanosleep_common(ns, (a2 & TIMER_ABSTIME) ? 0 : a4, 1);
}

/* --- resource limits --------------------------------------------------------------- */

#define RLIMIT_STACK  3
#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIM_INFINITY 0xFFFFFFFFFFFFFFFFull

static uint64_t limit_of(uint32_t resource)
{
    switch (resource) {
    case RLIMIT_STACK:  return USER_STACK_MAX;
    case RLIMIT_NPROC:  return PROC_MAX;
    case RLIMIT_NOFILE: return PROC_FILES;
    default:            return RLIM_INFINITY;
    }
}

SYSCALL_DEF(sys_getrlimit)              /* (resource, struct { u32 cur, max } *) */
{
    uint64_t v = limit_of(a1);
    uint32_t r[2];
    r[0] = r[1] = v > 0xFFFFFFFFull ? 0xFFFFFFFF : (uint32_t)v;
    return copy_to_user((void *)a2, r, sizeof r);
}

SYSCALL_DEF(sys_setrlimit)
{
    return 0;                           /* accepted; our limits are fixed */
}

SYSCALL_DEF(sys_prlimit64)              /* (pid, resource, new64 *, old64 *) */
{
    if (a4) {
        uint64_t r[2] = { limit_of(a2), limit_of(a2) };
        if (copy_to_user((void *)a4, r, sizeof r) < 0)
            return -EFAULT;
    }
    return 0;
}

/* --- system information ------------------------------------------------------------- */

SYSCALL_DEF(sys_sysinfo)
{
    struct {
        int32_t uptime;
        uint32_t loads[3];
        uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        uint16_t procs, pad;
        uint32_t totalhigh, freehigh, mem_unit;
        char reserved[8];
    } si;
    memset(&si, 0, sizeof si);
    si.uptime = (int32_t)(timer_ms() / 1000);
    si.totalram = pmm_usable_count();
    si.freeram = pmm_free_count();
    si.mem_unit = PAGE_SIZE;            /* counts are in pages */
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_at(i)->state != PROC_FREE)
            si.procs++;
    return copy_to_user((void *)a1, &si, sizeof si);
}

/* getrandom(buf, len, flags): not cryptographic — there's no entropy
   source yet — but different every call (cycle counter + xorshift). */
SYSCALL_DEF(sys_getrandom)
{
    static uint64_t s;
    s ^= cycles_now();
    uint32_t done = 0;
    while (done < a2) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        uint32_t n = a2 - done < 8 ? a2 - done : 8;
        if (copy_to_user((uint8_t *)a1 + done, &s, n) < 0)
            return done ? (int32_t)done : -EFAULT;
        done += n;
    }
    return done;
}

/* --- thread-local storage --------------------------------------------------------------- */

/* struct user_desc { entry_number, base_addr, limit, flags } */
SYSCALL_DEF(sys_set_thread_area)
{
    uint32_t d[4];
    if (copy_from_user(d, (void *)a1, sizeof d) < 0)
        return -EFAULT;
    struct thread *t = thread_current();
    int idx = (int)d[0];
    if (idx == -1) {                    /* "pick a free entry for me" */
        for (int i = 0; i < GDT_TLS_ENTRIES; i++)
            if (!t->tls[i]) {
                idx = GDT_TLS_FIRST + i;
                break;
            }
        if (idx == -1)
            return -ESRCH;
        d[0] = idx;
        if (copy_to_user((void *)a1, d, 4) < 0)
            return -EFAULT;
    }
    if (idx < GDT_TLS_FIRST || idx >= GDT_TLS_FIRST + GDT_TLS_ENTRIES)
        return -EINVAL;
    t->tls[idx - GDT_TLS_FIRST] = gdt_tls_descriptor(d[1], d[2], d[3]);
    thread_load_tls(t);
    return 0;
}

SYSCALL_DEF(sys_get_thread_area)
{
    uint32_t idx;
    if (get_user32(&idx, a1) < 0)
        return -EFAULT;
    if (idx < GDT_TLS_FIRST || idx >= GDT_TLS_FIRST + GDT_TLS_ENTRIES)
        return -EINVAL;
    uint64_t desc = thread_current()->tls[idx - GDT_TLS_FIRST];
    uint8_t *b = (uint8_t *)&desc;
    uint32_t d[4] = {
        idx,
        b[2] | (b[3] << 8) | (b[4] << 16) | ((uint32_t)b[7] << 24),     /* base */
        b[0] | (b[1] << 8) | ((b[6] & 0x0F) << 16),                     /* limit */
        ((b[6] >> 6) & 1) | ((!(b[5] & 2)) << 3) | (((b[6] >> 7) & 1) << 4) |
        ((!(b[5] & 0x80)) << 5) | (((b[6] >> 4) & 1) << 6),
    };
    return copy_to_user((void *)a1, d, sizeof d);
}

/* set_tid_address(int *tidptr): (for threads; we just answer the tid) */
SYSCALL_DEF(sys_set_tid_address)
{
    return proc_current()->pid;
}
