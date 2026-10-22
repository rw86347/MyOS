/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * sys_info.c — "who am I, what time is it" system calls, and thread-local
 * storage: the things every program asks the kernel when it starts.
 */
#include <errno.h>
#include <gdt.h>
#include <interrupts.h>
#include <kprintf.h>
#include <mm.h>
#include <pmm.h>
#include <proc.h>
#include <random.h>
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
   release string says what it really is. The second field, the host's
   name, is set by sethostname() (from /etc/hostname at boot). The release
   stays 6.1.0 (programs read it to know which calls exist); the version
   field (uname -v) names this build, from the Makefile. */
#ifndef MYOS_VERSION
#define MYOS_VERSION "unknown"
#endif
static char uts[6][65] = {
#ifdef ARCH_X86_64
    "Linux", "myos", "6.1.0-MyOS", "#1 MyOS " MYOS_VERSION, "x86_64", "(none)",
#else
    "Linux", "myos", "6.1.0-MyOS", "#1 MyOS " MYOS_VERSION, "i686", "(none)",
#endif
};

const char *uts_nodename(void) { return uts[1]; }

#define PER_LINUX32 0x0008

SYSCALL_DEF(sys_uname)
{
#ifdef ARCH_X86_64
    /* `linux32 prog` (personality PER_LINUX32): a 64-bit system that says
       it is a 32-bit one, so a build or installer picks i686. */
    if ((proc_current()->persona & 0xFF) == PER_LINUX32) {
        char u[6][65];
        memcpy(u, uts, sizeof u);
        memcpy(u[4], "i686", 5);
        return copy_to_user((void *)a1, u, sizeof u);
    }
#endif
    return copy_to_user((void *)a1, uts, sizeof uts);
}

/* sethostname(name, len): name need not end in a 0 byte. */
SYSCALL_DEF(sys_sethostname)
{
    char name[65];
    if (proc_current()->cred.euid != 0)
        return -EPERM;                  /* Linux: CAP_SYS_ADMIN, checked first */
    if (a2 > 64)
        return -EINVAL;
    if (copy_from_user(name, (const void *)a1, a2) < 0)
        return -EFAULT;
    memset(name + a2, 0, sizeof name - a2);
    memcpy(uts[1], name, sizeof name);
    return 0;
}

/* --- users and groups (cred.h) -------------------------------------------------- */

/*
 * The rules are Linux's. Root (euid 0) may set any id. Anyone else may only
 * move between the ids it already has (real, effective, saved): that's how
 * a setuid program drops its privilege and, through the saved id, takes it
 * back. -1 means "leave this one as it is". The file-access id follows the
 * effective one.
 */
#define KEEP 0xFFFFFFFFu

static struct cred *cred(void) { return &proc_current()->cred; }
static int is_root(void) { return cred()->euid == 0; }

static int one_of(uint32_t x, uint32_t a, uint32_t b, uint32_t c)
{
    return x == a || x == b || x == c;
}

SYSCALL_DEF(sys_getuid)  { return cred()->uid; }
SYSCALL_DEF(sys_geteuid) { return cred()->euid; }
SYSCALL_DEF(sys_getgid)  { return cred()->gid; }
SYSCALL_DEF(sys_getegid) { return cred()->egid; }

/* setresuid(r, e, s) and setresgid: the general one. */
static int32_t set_res(uint32_t *r, uint32_t *e, uint32_t *s, uint32_t *fs,
                       uint32_t nr, uint32_t ne, uint32_t ns)
{
    if (!is_root() && ((nr != KEEP && !one_of(nr, *r, *e, *s)) ||
                       (ne != KEEP && !one_of(ne, *r, *e, *s)) ||
                       (ns != KEEP && !one_of(ns, *r, *e, *s))))
        return -EPERM;
    if (nr != KEEP) *r = nr;
    if (ne != KEEP) *e = ne;
    if (ns != KEEP) *s = ns;
    *fs = *e;
    return 0;
}

SYSCALL_DEF(sys_setresuid)
{
    struct cred *c = cred();
    return set_res(&c->uid, &c->euid, &c->suid, &c->fsuid, a1, a2, a3);
}

SYSCALL_DEF(sys_setresgid)
{
    struct cred *c = cred();
    return set_res(&c->gid, &c->egid, &c->sgid, &c->fsgid, a1, a2, a3);
}

/* setreuid(r, e) and setregid: the real id may become the effective one,
   the effective any of the three. Changing the real id, or the effective
   to something other than the real, also sets the saved id. */
static int32_t set_re(uint32_t *r, uint32_t *e, uint32_t *s, uint32_t *fs,
                      uint32_t nr, uint32_t ne)
{
    if (!is_root() && ((nr != KEEP && nr != *r && nr != *e) ||
                       (ne != KEEP && !one_of(ne, *r, *e, *s))))
        return -EPERM;
    uint32_t old_r = *r;
    if (nr != KEEP) *r = nr;
    if (ne != KEEP) *e = ne;
    if (nr != KEEP || (ne != KEEP && ne != old_r))
        *s = *e;
    *fs = *e;
    return 0;
}

SYSCALL_DEF(sys_setreuid)
{
    struct cred *c = cred();
    return set_re(&c->uid, &c->euid, &c->suid, &c->fsuid, a1, a2);
}

SYSCALL_DEF(sys_setregid)
{
    struct cred *c = cred();
    return set_re(&c->gid, &c->egid, &c->sgid, &c->fsgid, a1, a2);
}

/* setuid(id) and setgid: root sets all of them (a login dropping to the
   user for good), anyone else only the effective one. */
static int32_t set_one(uint32_t *r, uint32_t *e, uint32_t *s, uint32_t *fs, uint32_t id)
{
    if (is_root())
        *r = *s = id;
    else if (!one_of(id, *r, *e, *s))
        return -EPERM;
    *e = *fs = id;
    return 0;
}

SYSCALL_DEF(sys_setuid)
{
    struct cred *c = cred();
    return set_one(&c->uid, &c->euid, &c->suid, &c->fsuid, a1);
}

SYSCALL_DEF(sys_setgid)
{
    struct cred *c = cred();
    return set_one(&c->gid, &c->egid, &c->sgid, &c->fsgid, a1);
}

/* setfsuid(id) / setfsgid: returns the old one; fails silently. */
SYSCALL_DEF(sys_setfsuid)
{
    struct cred *c = cred();
    uint32_t old = c->fsuid;
    if (is_root() || one_of(a1, c->uid, c->euid, c->suid))
        c->fsuid = a1;
    return old;
}

SYSCALL_DEF(sys_setfsgid)
{
    struct cred *c = cred();
    uint32_t old = c->fsgid;
    if (is_root() || one_of(a1, c->gid, c->egid, c->sgid))
        c->fsgid = a1;
    return old;
}

/* getresuid(&r, &e, &s) / getresgid */
static int32_t get_res(uint32_t r, uint32_t e, uint32_t s, uintptr_t ur, uintptr_t ue, uintptr_t us)
{
    if (copy_to_user((void *)ur, &r, 4) < 0 || copy_to_user((void *)ue, &e, 4) < 0 ||
        copy_to_user((void *)us, &s, 4) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL_DEF(sys_getresuid)
{
    struct cred *c = cred();
    return get_res(c->uid, c->euid, c->suid, a1, a2, a3);
}

SYSCALL_DEF(sys_getresgid)
{
    struct cred *c = cred();
    return get_res(c->gid, c->egid, c->sgid, a1, a2, a3);
}

/* getpriority(which, who) / setpriority(which, who, nice): "nice" values,
   per process. which: 0 a process (who 0: me), 1 a process group, 2 a
   user's processes. Only root may make one nicer than it is (lower);
   anyone may set their own processes' higher. getpriority returns 20 - nice
   (1..40), so the C library can tell it from an error. */
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

static int prio_match(struct process *p, uint32_t which, uint32_t who)
{
    struct process *me = proc_current();
    if (p->state != PROC_ALIVE)
        return 0;
    switch (which) {
    case PRIO_PROCESS: return p->pid == (int)(who ? who : (uint32_t)me->pid);
    case PRIO_PGRP:    return p->pgid == (int)(who ? who : (uint32_t)me->pgid);
    case PRIO_USER:    return p->cred.uid == (who ? who : me->cred.uid);
    }
    return 0;
}

SYSCALL_DEF(sys_getpriority)
{
    if (a1 > PRIO_USER)
        return -EINVAL;
    int best = 21, found = 0;           /* the most favoured of them */
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (prio_match(p, a1, a2)) {
            found = 1;
            if (p->nice < best)
                best = p->nice;
        }
    }
    return found ? 20 - best : -ESRCH;
}

SYSCALL_DEF(sys_setpriority)
{
    if (a1 > PRIO_USER)
        return -EINVAL;
    int nice = (int32_t)a3 < -20 ? -20 : (int32_t)a3 > 19 ? 19 : (int32_t)a3;
    struct cred *me = cred();
    int found = 0, err = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (!prio_match(p, a1, a2))
            continue;
        found = 1;
        if (!is_root() && me->euid != p->cred.uid && me->euid != p->cred.euid)
            err = -EPERM;               /* someone else's process */
        else if (!is_root() && nice < p->nice)
            err = -EACCES;              /* only root makes it more favoured */
        else
            p->nice = nice;
    }
    return found ? err : -ESRCH;
}

/* getgroups(size, list): size 0 asks how many. */
SYSCALL_DEF(sys_getgroups)
{
    struct cred *c = cred();
    if (a1 == 0)
        return c->ngroups;
    if (a1 < c->ngroups)
        return -EINVAL;
    if (copy_to_user((void *)a2, c->groups, c->ngroups * 4) < 0)
        return -EFAULT;
    return c->ngroups;
}

/* setgroups(size, list): root only (login and su call it). */
SYSCALL_DEF(sys_setgroups)
{
    uint32_t groups[NGROUPS];
    if (!is_root())
        return -EPERM;
    if (a1 > NGROUPS)
        return -EINVAL;
    if (copy_from_user(groups, (const void *)a2, a1 * 4) < 0)
        return -EFAULT;
    struct cred *c = cred();
    memcpy(c->groups, groups, a1 * 4);
    c->ngroups = a1;
    return 0;
}

/* --- time ------------------------------------------------------------------------ */

/* The wall clock: what it said at boot (ns since 1970: the CMOS clock, moved
   by clock_settime and adjtimex), plus the 1 ms timer since. (Millisecond
   resolution for now; a vDSO with nanoseconds is step 11.) Read and written
   with interrupts off: on i386 a 64-bit value is two stores. */
static int64_t boot_realtime_ns;

void time_init(void)
{
    boot_realtime_ns = (int64_t)rtc_read_epoch() * 1000000000;
}

static int64_t realtime_ns(void)
{
    uint32_t flags = irq_save();
    int64_t ns = boot_realtime_ns + (int64_t)timer_ms() * 1000000;
    irq_restore(flags);
    return ns;
}

/* Set the wall clock to ns (since 1970). Monotonic time doesn't move. */
static void set_realtime_ns(int64_t ns)
{
    uint32_t flags = irq_save();
    boot_realtime_ns = ns - (int64_t)timer_ms() * 1000000;
    irq_restore(flags);
}

uint32_t time_now(void)
{
    return (uint32_t)(realtime_ns() / 1000000000);
}

#define CLOCK_REALTIME          0
#define CLOCK_REALTIME_COARSE   5
#define CLOCK_REALTIME_ALARM    8
#define TIMER_ABSTIME           1

/* Nanoseconds on clock clk: realtime (since 1970) or monotonic (boot). */
int64_t clock_ns(uint32_t clk)
{
    if (clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE || clk == CLOCK_REALTIME_ALARM)
        return realtime_ns();
    return (int64_t)timer_ms() * 1000000;   /* monotonic, boottime, cputime... */
}

/* time_t time(time_t *t): a long, 32 bits on i386, 64 on x86_64. */
SYSCALL_DEF(sys_time)
{
    long t = (long)(clock_ns(CLOCK_REALTIME) / 1000000000);
    if (a1 && copy_to_user((void *)a1, &t, sizeof t) < 0)
        return -EFAULT;
    return t;
}

/* gettimeofday(struct timeval *, struct timezone *): timeval is two longs. */
SYSCALL_DEF(sys_gettimeofday)
{
    int64_t ns = clock_ns(CLOCK_REALTIME);
    long tv[2] = { (long)(ns / 1000000000), (long)(ns % 1000000000 / 1000) };
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

/* clock_settime(clock, const struct timespec *) with 64-bit seconds (musl
   uses this one on i386; on x86_64 it is the only one): set the wall clock.
   Only root; only CLOCK_REALTIME (the others can't be set, as on Linux).
   `date -s`, `hwclock -s` and ntpd's steps come here. */
SYSCALL_DEF(sys_clock_settime64)
{
    int64_t ts[2];
    if (a1 != CLOCK_REALTIME)
        return -EINVAL;
    if (!is_root())
        return -EPERM;
    if (copy_from_user(ts, (void *)a2, sizeof ts) < 0)
        return -EFAULT;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000)
        return -EINVAL;
    set_realtime_ns(ts[0] * 1000000000 + ts[1]);
    return 0;
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

static int32_t nanosleep_common(int64_t ns, uintptr_t user_rem, int wide)
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

/* --- personality, capabilities, namespaces, swap ------------------------------- */

/* personality(persona): which Unix the program wants the kernel to be. Only
   Linux's own; the flag bits (ADDR_NO_RANDOMIZE, ...) are kept — MyOS has no
   address randomisation to turn off anyway — and PER_LINUX32 changes uname
   on x86_64. 0xffffffff only asks. Returns the old one. */
SYSCALL_DEF(sys_personality)
{
    struct process *p = proc_current();
    uint32_t old = p->persona;
    if ((uint32_t)a1 != 0xFFFFFFFF) {
        uint32_t kind = (uint32_t)a1 & 0xFF;
        if (kind != 0 && kind != PER_LINUX32)
            return -EINVAL;
        p->persona = (uint32_t)a1;
    }
    return old;
}

/*
 * capget / capset. Linux splits root's power into capabilities; MyOS keeps
 * the classic rule (euid 0 may do everything), so root has all of them and
 * everyone else none — which is what capget reports, version 3 layout. A
 * capset that would DROP capabilities is refused (EPERM): the kernel could
 * not enforce it, and a program that drops privileges and then trusts that
 * they are gone must not be told they are. Setting what is already true is
 * allowed (and changes nothing).
 */
#define CAP_V1 0x19980330
#define CAP_V2 0x20071026
#define CAP_V3 0x20080522
#define CAP_LAST 40                     /* CAP_CHECKPOINT_RESTORE */

static int cap_header(uintptr_t uhdr, int *words)
{
    uint32_t hdr[2];                    /* version, pid */
    if (copy_from_user(hdr, (void *)uhdr, sizeof hdr) < 0)
        return -EFAULT;
    if (hdr[0] != CAP_V1 && hdr[0] != CAP_V2 && hdr[0] != CAP_V3) {
        uint32_t v = CAP_V3;            /* tell the caller what we speak */
        if (copy_to_user((void *)uhdr, &v, 4) < 0)
            return -EFAULT;
        return -EINVAL;
    }
    if ((int32_t)hdr[1] < 0)
        return -EINVAL;
    if (hdr[1] && !proc_find(hdr[1]))
        return -ESRCH;
    *words = hdr[0] == CAP_V1 ? 1 : 2;
    return (int32_t)hdr[1];
}

/* What a process has: all for root, nothing otherwise. (Other processes:
   by their effective user id.) */
static void caps_of(int pid, uint32_t data[6])
{
    struct process *p = pid ? proc_find(pid) : proc_current();
    uint32_t all0 = 0xFFFFFFFF, all1 = (1u << (CAP_LAST - 31)) - 1;
    int root = p && p->cred.euid == 0;
    for (int w = 0; w < 2; w++) {
        uint32_t v = root ? (w ? all1 : all0) : 0;
        data[w * 3 + 0] = v;            /* effective */
        data[w * 3 + 1] = v;            /* permitted */
        data[w * 3 + 2] = 0;            /* inheritable */
    }
}

SYSCALL_DEF(sys_capget)
{
    int words, pid = cap_header(a1, &words);
    if (pid < 0)
        return a2 || pid != -EINVAL ? pid : 0;  /* NULL data: just the version */
    if (!a2)
        return 0;
    uint32_t data[6];
    caps_of(pid, data);
    return copy_to_user((void *)a2, data, words * 3 * sizeof(uint32_t));
}

SYSCALL_DEF(sys_capset)
{
    int words, pid = cap_header(a1, &words);
    if (pid < 0)
        return pid;
    if (pid && pid != proc_current()->pid)
        return -EPERM;                  /* only one's own, as on Linux */
    uint32_t want[6], have[6];
    if (copy_from_user(want, (void *)a2, words * 3 * sizeof(uint32_t)) < 0)
        return -EFAULT;
    caps_of(0, have);
    for (int i = 0; i < words * 3; i++)
        if (want[i] != have[i])
            return -EPERM;              /* a change we could not keep */
    return 0;
}

/* unshare(flags): Linux's namespaces aren't here (like a Linux built without
   them: CLONE_NEW* is EINVAL). What is left to unshare — the files table,
   cwd and umask, SysV semaphore undo — is already this process's own unless
   it has other threads. */
#define CLONE_FS       0x00000200
#define CLONE_FILES    0x00000400
#define CLONE_SYSVSEM  0x00040000
SYSCALL_DEF(sys_unshare)
{
    if (a1 & ~(uintptr_t)(CLONE_FS | CLONE_FILES | CLONE_SYSVSEM))
        return -EINVAL;
    if ((a1 & (CLONE_FS | CLONE_FILES)) && proc_current()->nthreads > 1)
        return -EINVAL;                 /* threads share them: can't split yet */
    return 0;
}

/* setns(fd, type): no namespaces, so no namespace file to join. */
SYSCALL_DEF(sys_setns)
{
    return fd_get(a1) ? -EINVAL : -EBADF;
}

/* swapon / swapoff: MyOS has no swap, by design — a page fault never waits
   for the disk to make room, which keeps latency predictable. This is the
   answer of a Linux built without swap (CONFIG_SWAP=n). */
SYSCALL_DEF(sys_swapon)  { return is_root() ? -ENOSYS : -EPERM; }

/* --- adjtimex / clock_adjtime ------------------------------------------------- */
/*
 * What ntpd and `adjtimex` read and set. Linux runs a phase-locked loop that
 * slews the clock a little each tick; MyOS has none (its clock is the 1 ms
 * timer), so: an offset to correct (ADJ_OFFSET, adjtime's one-shot,
 * ADJ_SETOFFSET) is applied at once as a step, and the frequency, status,
 * error estimates and so on are kept and read back but steer nothing. Reading
 * is for anyone; any change is root's.
 */
#define USER_HZ 100                     /* clock_t ticks per second, as on Linux */

#define ADJ_OFFSET          0x0001
#define ADJ_FREQUENCY       0x0002
#define ADJ_MAXERROR        0x0004
#define ADJ_ESTERROR        0x0008
#define ADJ_STATUS          0x0010
#define ADJ_TIMECONST       0x0020
#define ADJ_TAI             0x0080
#define ADJ_SETOFFSET       0x0100
#define ADJ_MICRO           0x1000
#define ADJ_NANO            0x2000
#define ADJ_TICK            0x4000
#define ADJ_OFFSET_SINGLESHOT 0x8001    /* adjtime() */
#define ADJ_OFFSET_SS_READ  0xa001      /* adjtime() just asking */
#define STA_UNSYNC          0x0040
#define STA_NANO            0x2000
#define TIME_OK             0
#define TIME_ERROR          5

/* One struct timex, with every field 64-bit; converted from and to the two
   layouts programs use. */
struct ktimex {
    uint32_t modes;
    int32_t status, shift, tai;
    int64_t offset, freq, maxerror, esterror, constant, precision, tolerance;
    int64_t sec, frac;                  /* time: frac in us, or ns with STA_NANO */
    int64_t tick, ppsfreq, jitter, stabil, jitcnt, calcnt, errcnt, stbcnt;
};

static struct {
    int32_t status, tai;
    int64_t freq, maxerror, esterror, constant, tick;
} ntp = { STA_UNSYNC, 0, 0, 16000000, 16000000, 2, 1000000 / USER_HZ };

static int do_adjtimex(struct ktimex *t)
{
    uint32_t m = t->modes;
    if (m == ADJ_OFFSET_SS_READ)
        m = 0;
    if (m && !is_root())
        return -EPERM;
    int nano = (ntp.status & STA_NANO) != 0;
    if (m & ADJ_SETOFFSET) {            /* step by the time field */
        int64_t frac = t->frac;
        int ns = (m & ADJ_NANO) != 0;
        if (frac < 0 || frac >= (ns ? 1000000000 : 1000000))
            return -EINVAL;
        set_realtime_ns(realtime_ns() + t->sec * 1000000000 + (ns ? frac : frac * 1000));
    }
    if ((m & ADJ_OFFSET) == ADJ_OFFSET) {   /* also adjtime(): step, don't slew */
        int64_t off = t->offset;
        int ns = (m != ADJ_OFFSET_SINGLESHOT) && nano;
        set_realtime_ns(realtime_ns() + (ns ? off : off * 1000));
    }
    if (m & ADJ_FREQUENCY)
        ntp.freq = t->freq;
    if (m & ADJ_MAXERROR)
        ntp.maxerror = t->maxerror;
    if (m & ADJ_ESTERROR)
        ntp.esterror = t->esterror;
    if (m & ADJ_STATUS)
        ntp.status = (ntp.status & STA_NANO) | (t->status & ~STA_NANO);
    if (m & ADJ_TIMECONST)
        ntp.constant = t->constant;
    if (m & ADJ_TAI)
        ntp.tai = (int32_t)t->constant;    /* (as Linux: the value comes in constant) */
    if (m & ADJ_TICK)
        ntp.tick = t->tick;
    if (m & ADJ_NANO)
        ntp.status |= STA_NANO;
    if (m & ADJ_MICRO)
        ntp.status &= ~STA_NANO;

    nano = (ntp.status & STA_NANO) != 0;
    int64_t now = realtime_ns();
    memset(t, 0, sizeof *t);
    t->modes = m;
    t->status = ntp.status;
    t->tai = ntp.tai;
    t->freq = ntp.freq;
    t->maxerror = ntp.maxerror;
    t->esterror = ntp.esterror;
    t->constant = ntp.constant;
    t->precision = 1000;                /* us: the clock moves in 1 ms steps */
    t->tolerance = 500ll << 16;         /* 500 ppm, as Linux reports */
    t->tick = ntp.tick;
    t->sec = now / 1000000000;
    t->frac = nano ? now % 1000000000 : now % 1000000000 / 1000;
    return (ntp.status & STA_UNSYNC) ? TIME_ERROR : TIME_OK;
}

/* struct timex with longs: adjtimex and clock_adjtime on x86_64, and the old
   ones on i386. The compiler pads it as Linux does on both. */
struct timex_long {
    uint32_t modes;
    long offset, freq, maxerror, esterror;
    int32_t status;
    long constant, precision, tolerance, sec, frac, tick, ppsfreq, jitter;
    int32_t shift;
    long stabil, jitcnt, calcnt, errcnt, stbcnt;
    int32_t tai;
    int32_t reserved[11];
};

/* The 64-bit-time one (i386 clock_adjtime64): x86_64's layout, spelled out. */
struct timex_64 {
    uint32_t modes;
    int32_t pad0;
    int64_t offset, freq, maxerror, esterror;
    int32_t status, pad1;
    int64_t constant, precision, tolerance, sec, frac, tick, ppsfreq, jitter;
    int32_t shift, pad2;
    int64_t stabil, jitcnt, calcnt, errcnt, stbcnt;
    int32_t tai;
    int32_t reserved[11];
};

/* Copy the fields named in both layouts (a macro: the two structs differ
   only in field widths). */
#define TIMEX_FIELDS(X) X(modes) X(offset) X(freq) X(maxerror) X(esterror) X(status) \
    X(constant) X(precision) X(tolerance) X(sec) X(frac) X(tick) X(ppsfreq) X(jitter) \
    X(shift) X(stabil) X(jitcnt) X(calcnt) X(errcnt) X(stbcnt) X(tai)

static intptr_t adjtimex_user(uintptr_t user, int wide)
{
    struct ktimex k;
    int rc;
    if (wide) {
        struct timex_64 u;
        if (copy_from_user(&u, (void *)user, sizeof u) < 0)
            return -EFAULT;
#define IN(f) k.f = u.f;
#define OUT(f) u.f = k.f;
        TIMEX_FIELDS(IN)
        if ((rc = do_adjtimex(&k)) < 0)
            return rc;
        TIMEX_FIELDS(OUT)
        return copy_to_user((void *)user, &u, sizeof u) < 0 ? -EFAULT : rc;
    }
    struct timex_long u;
    if (copy_from_user(&u, (void *)user, sizeof u) < 0)
        return -EFAULT;
    TIMEX_FIELDS(IN)
    if ((rc = do_adjtimex(&k)) < 0)
        return rc;
    TIMEX_FIELDS(OUT)
#undef IN
#undef OUT
    return copy_to_user((void *)user, &u, sizeof u) < 0 ? -EFAULT : rc;
}

SYSCALL_DEF(sys_adjtimex)       { return adjtimex_user(a1, 0); }
SYSCALL_DEF(sys_clock_adjtime)  { return a1 == CLOCK_REALTIME ? adjtimex_user(a2, 0) : -EOPNOTSUPP; }
SYSCALL_DEF(sys_clock_adjtime64){ return a1 == CLOCK_REALTIME ? adjtimex_user(a2, 1) : -EOPNOTSUPP; }

/* --- CPU time: times, getrusage --------------------------------------------- */


/* A process's own CPU time in ms: its ended threads' and its live ones'. */
static void self_times(struct process *p, uint32_t *user, uint32_t *sys)
{
    uint32_t flags = irq_save();
    *user = p->utime;
    *sys = p->stime;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        *user += t->utime;
        *sys += t->stime;
    }
    irq_restore(flags);
}

/* clock_t times(struct tms *buf): the process's and its waited-for children's
   CPU time in clock ticks; returns ticks since boot. struct tms is four
   longs: 32 bits on i386, 64 on x86_64, as the C library expects. */
SYSCALL_DEF(sys_times)
{
    struct process *p = proc_current();
    uint32_t user, sys;
    self_times(p, &user, &sys);
    long tms[4] = { (long)(user / (1000 / USER_HZ)), (long)(sys / (1000 / USER_HZ)),
                    (long)(p->cutime / (1000 / USER_HZ)), (long)(p->cstime / (1000 / USER_HZ)) };
    if (a1 && copy_to_user((void *)a1, tms, sizeof tms) < 0)
        return -EFAULT;
    return (long)(timer_ms() / (1000 / USER_HZ));
}

/* struct rusage: user and system time (struct timevals: two longs each),
   then 14 long counters, of which only ru_minflt is counted here (maxrss,
   major faults, context switches...: 0). */
int rusage_to_user(uintptr_t user, uint32_t user_ms, uint32_t sys_ms, uint32_t minflt)
{
    long ru[18];
    memset(ru, 0, sizeof ru);
    ru[8] = (long)minflt;               /* ru_minflt */
    ru[0] = (long)(user_ms / 1000);
    ru[1] = (long)(user_ms % 1000) * 1000;
    ru[2] = (long)(sys_ms / 1000);
    ru[3] = (long)(sys_ms % 1000) * 1000;
    return copy_to_user((void *)user, ru, sizeof ru);
}

#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD    1

/* int getrusage(int who, struct rusage *ru) */
SYSCALL_DEF(sys_getrusage)
{
    struct process *p = proc_current();
    struct thread *t = thread_current();
    uint32_t user, sys, flt = p->minflt;   /* (the thread's: its process's) */
    switch ((int32_t)a1) {
    case RUSAGE_SELF:     self_times(p, &user, &sys); break;
    case RUSAGE_CHILDREN: user = p->cutime; sys = p->cstime; flt = p->cminflt; break;
    case RUSAGE_THREAD:   user = t->utime; sys = t->stime; break;
    default:              return -EINVAL;
    }
    return rusage_to_user(a2, user, sys, flt);
}

SYSCALL_DEF(sys_sysinfo)
{
    struct {                            /* Linux's: longs are 32 or 64 bits */
        long uptime;
        unsigned long loads[3];
        unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        uint16_t procs, pad;
        unsigned long totalhigh, freehigh;
        uint32_t mem_unit;
        char reserved[20 - 2 * sizeof(long) - sizeof(uint32_t)];   /* (none on x86_64) */
    } si;
    memset(&si, 0, sizeof si);
    si.uptime = (long)(timer_ms() / 1000);
    si.totalram = pmm_usable_count();
    si.freeram = pmm_free_count();
    si.mem_unit = PAGE_SIZE;            /* counts are in pages */
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_at(i)->state != PROC_FREE)
            si.procs++;
    return copy_to_user((void *)a1, &si, sizeof si);
}

/* getrandom(buf, len, flags): ChaCha20, see kernel/random.c. Copied out in
   chunks so a huge length cannot hold the CPU in one syscall, and the chunk
   is wiped afterwards so the next caller cannot be handed it from the stack
   if the copy faults halfway. */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

SYSCALL_DEF(sys_getrandom)
{
    if (a3 & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
        return -EINVAL;
    if ((a3 & GRND_RANDOM) && (a3 & GRND_INSECURE))
        return -EINVAL;             /* contradictory, as Linux says */
    /* The flags otherwise change nothing: there is one source, it is always
       ready, and it never blocks, so GRND_NONBLOCK has nothing to not-wait
       for and GRND_INSECURE has nothing weaker to offer. */
    uint8_t buf[64];
    uint32_t done = 0;
    while (done < a2) {
        /* A caller may ask for a gigabyte, and a syscall that works through it
           in one go would hold the CPU for millions of ChaCha20 blocks and
           blow its scheduler slot. So this is interruptible and returns what
           it managed, which is what getrandom() promises. Checked a page at a
           time, as Linux does; requests of 256 bytes or less — every real one —
           finish in one pass and cannot come back short. */
        if (done && !(done % 4096) && signal_pending())
            return (int32_t)done;
        uint32_t n = a2 - done < sizeof buf ? a2 - done : sizeof buf;
        random_bytes(buf, n);
        int rc = copy_to_user((uint8_t *)a1 + done, buf, n);
        memset(buf, 0, n);          /* do not leave it on the kernel stack */
        if (rc < 0)
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
    /* musl loads GS itself after the syscall; also put the selector in
       the saved frame so `pop gs` on the way out does not undo it. */
    thread_set_saved_gs(t, (uint32_t)((idx << 3) | 3));
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

/* set_tid_address(int *tidptr): when this thread ends, write 0 to *tidptr
   and futex-wake it (how pthread_join learns a thread is gone). */
SYSCALL_DEF(sys_set_tid_address)
{
    struct thread *t = thread_current();
    t->clear_tid = a1;
    return t->tid;
}
