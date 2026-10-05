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
   name, is set by sethostname() (from /etc/hostname at boot). */
static char uts[6][65] = {
    "Linux", "myos", "6.1.0-MyOS", "#1 MyOS", "i686", "(none)",
};

SYSCALL_DEF(sys_uname)
{
    return copy_to_user((void *)a1, uts, sizeof uts);
}

/* sethostname(name, len): name need not end in a 0 byte. */
SYSCALL_DEF(sys_sethostname)
{
    char name[65];
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
static int32_t get_res(uint32_t r, uint32_t e, uint32_t s, uint32_t ur, uint32_t ue, uint32_t us)
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
int64_t clock_ns(uint32_t clk)
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
