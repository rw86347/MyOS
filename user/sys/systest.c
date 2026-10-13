/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * systest.c — the system calls added on 2026-10-05, checked from a program's
 * side, on either CPU (built for whichever ARCH is; the Makefile's
 * test-uefi-shell runs it). No C library: raw `syscall` (x86_64) or
 * `int 0x80` (i386), Linux's numbers and struct layouts written out here
 * from the Linux headers — so a struct the kernel lays out wrong shows up.
 *
 * Prints "ok <name>" or "FAIL <name> ..." per check, then
 * "systest: all ok" or "systest: N failed".
 */
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)
#define X64 1
/* x86_64 numbers (syscall_64.tbl) */
enum { S_read = 0, S_write = 1, S_open = 2, S_close = 3, S_lseek = 8, S_mmap = 9,
       S_munmap = 11, S_rt_sigaction = 13, S_rt_sigreturn = 15, S_pipe = 22,
       S_select = 23, S_shmget = 29, S_shmat = 30, S_shmctl = 31, S_pause = 34,
       S_setitimer = 38, S_getpid = 39, S_sendfile = 40, S_fork = 57, S_exit = 60,
       S_wait4 = 61, S_kill = 62, S_uname = 63, S_semget = 64, S_semop = 65,
       S_semctl = 66, S_shmdt = 67, S_msgget = 68, S_msgsnd = 69, S_msgrcv = 70,
       S_msgctl = 71, S_fcntl = 72, S_getcwd = 79, S_chdir = 80, S_mkdir = 83,
       S_rmdir = 84, S_unlink = 87, S_gettimeofday = 96, S_getrusage = 98,
       S_sysinfo = 99, S_times = 100, S_syslog = 103, S_capget = 125,
       S_personality = 135, S_statfs = 137, S_adjtimex = 159, S_chroot = 161,
       S_time = 201, S_fadvise = 221, S_clock_gettime = 228, S_exit_group = 231,
       S_inotify_add_watch = 254, S_inotify_rm_watch = 255, S_unshare = 272,
       S_inotify_init1 = 294, S_syncfs = 306, S_copy_file_range = 326,
       S_getxattr = 191, S_fsetxattr = 190, S_flistxattr = 196,
       S_ioctl = 16, S_socket = 41, S_sendto = 44, S_recvfrom = 45, S_bind = 49, S_setsockopt = 54,
       S_getsockname = 51, S_connect = 42, S_listen = 50, S_accept4 = 288, S_poll = 7,
       S_setuid = 105, S_sigaltstack = 131, S_socketpair = 53, S_sendmsg = 46,
       S_recvmsg = 47, S_getsockopt = 55, S_gettid = 186, S_rt_sigqueueinfo = 129,
       S_rt_tgsigqueueinfo = 297, S_epoll_create1 = 291, S_epoll_ctl = 233,
       S_reboot = 169, S_sethostname = 170, S_tgkill = 234,
       S_ppoll = 271, S_mprotect = 10,
       S_access = 21, S_chmod = 90, S_chown = 92, S_truncate = 76, S_rename = 82,
       S_setresuid = 117, S_utimensat = 280, S_faccessat2 = 439,
       S_setgroups = 116, S_setgid = 106,
       S_mount = 165, S_umount2 = 166, S_nanosleep = 35 };
enum { F_GETLK = 5, F_SETLK = 6 };
#else
#define X64 0
/* i386 numbers (syscall_32.tbl); musl's choices where there are several */
enum { S_read = 3, S_write = 4, S_open = 5, S_close = 6, S_lseek = 19, S_mmap = 192,
       S_munmap = 91, S_rt_sigaction = 174, S_rt_sigreturn = 173, S_pipe = 42,
       S_select = 142, S_shmget = 395, S_shmat = 397, S_shmctl = 396, S_pause = 29,
       S_setitimer = 104, S_getpid = 20, S_sendfile = 239, S_fork = 2, S_exit = 1,
       S_wait4 = 114, S_kill = 37, S_uname = 122, S_ipc = 117, S_shmdt = 398,
       S_fcntl = 221, S_getcwd = 183, S_chdir = 12, S_mkdir = 39, S_rmdir = 40,
       S_unlink = 10, S_gettimeofday = 78, S_getrusage = 77, S_sysinfo = 116,
       S_times = 43, S_syslog = 103, S_capget = 184, S_personality = 136,
       S_statfs = 268, S_adjtimex = 124, S_chroot = 61, S_time = 13,
       S_fadvise = 272, S_clock_gettime = 403, S_exit_group = 252,
       S_inotify_add_watch = 292, S_inotify_rm_watch = 293, S_unshare = 310,
       S_inotify_init1 = 332, S_syncfs = 344, S_copy_file_range = 377,
       S_getxattr = 229, S_fsetxattr = 228, S_flistxattr = 234,
       S_ioctl = 54, S_socket = 359, S_bind = 361, S_getsockname = 367,
       S_sendto = 369, S_recvfrom = 371, S_setsockopt = 366, S_connect = 362,
       S_listen = 363, S_accept4 = 364, S_poll = 168, S_setuid = 213 /* setuid32 */,
       S_sigaltstack = 186, S_socketpair = 360, S_sendmsg = 370, S_recvmsg = 372,
       S_getsockopt = 365, S_gettid = 224, S_rt_sigqueueinfo = 178,
       S_rt_tgsigqueueinfo = 335, S_epoll_create1 = 329, S_epoll_ctl = 255,
       S_reboot = 88, S_sethostname = 74, S_tgkill = 270,
       S_ppoll = 309 /* timespec32 */, S_mprotect = 125,
       S_access = 33, S_chmod = 15, S_chown = 212 /* chown32 */, S_truncate = 92,
       S_rename = 38, S_setresuid = 208 /* setresuid32 */, S_utimensat = 320,
       S_faccessat2 = 439, S_setgroups = 206 /* setgroups32 */,
       S_setgid = 214 /* setgid32 */, S_mount = 21, S_umount2 = 52, S_nanosleep = 162 };
enum { F_GETLK = 12, F_SETLK = 13 };    /* the flock64 ones (musl) */
#endif

#if X64
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}
__asm__(".globl _start\n_start:\n"
        "  xor %rbp, %rbp\n  and $-16, %rsp\n  call cmain\n  hlt\n"
        "restore_rt:\n  mov $15, %eax\n  syscall\n");
#else
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;                           /* (the 6th goes in EBP: musl's way) */
    __asm__ volatile ("pushl %7\n\tpush %%ebp\n\tmov 4(%%esp), %%ebp\n\t"
                      "int $0x80\n\tpop %%ebp\n\tadd $4, %%esp"
                      : "=a"(ret)
                      : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "g"(f)
                      : "memory");
    return ret;
}
/* Two restorers on i386, as in musl: rt_sigreturn for SA_SIGINFO handlers
   (an rt frame), the old sigreturn (which first pops `sig`) for the rest. */
__asm__(".globl _start\n_start:\n"
        "  xor %ebp, %ebp\n  and $-16, %esp\n  call cmain\n  hlt\n"
        "restore_rt:\n  mov $173, %eax\n  int $0x80\n"
        "restore:\n  pop %eax\n  mov $119, %eax\n  int $0x80\n");
#endif
extern char restore_rt[], restore[];

#define SC0(n)             sc(n, 0, 0, 0, 0, 0, 0)
#define SC1(n, a)          sc(n, (long)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b)       sc(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c)    sc(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SC4(n, a, b, c, d) sc(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define SC5(n, a, b, c, d, e) sc(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define SC6(n, a, b, c, d, e, f) sc(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

/* --- tiny helpers ---------------------------------------------------------- */

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { SC3(S_write, 1, s, slen(s)); }
static void outn(long v)
{
    char b[24], *p = b + sizeof b;
    int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    *--p = 0;
    do { *--p = '0' + u % 10; u /= 10; } while (u);
    if (neg) *--p = '-';
    out(p);
}
static int same(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static void fill(void *p, int c, size_t n) { unsigned char *q = p; while (n--) *q++ = (unsigned char)c; }
static int contains(const char *h, size_t n, const char *needle)
{
    size_t m = slen(needle);
    for (size_t i = 0; i + m <= n; i++) if (same(h + i, needle, m)) return 1;
    return 0;
}

static int failed;
static void check(int ok, const char *name, long detail)
{
    if (ok) { out("ok "); out(name); out("\n"); return; }
    failed++;
    out("FAIL "); out(name); out(" ("); outn(detail); out(")\n");
}

static long now_ms(void)
{
    int64_t ts[2];                      /* 64-bit timespec on both */
    SC2(S_clock_gettime, 1, ts);
    return (long)(ts[0] * 1000 + ts[1] / 1000000);
}

static void child_exit(int code) { SC1(S_exit_group, code); }
static int wait_child(long pid)
{
    int st = 0;
    SC4(S_wait4, pid, &st, 0, 0);
    return st;
}

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define IPC_PRIVATE 0
#define IPC_CREAT 01000
#define IPC_NOWAIT 04000
#define IPC_RMID 0
#define IPC_STAT 2
#define IPC_64 0x100
#define EPERM 1
#define ESRCH 3
#define EAGAIN 11
#define EINTR 4
#define EACCES 13
#define EINVAL 22
#define ENOMSG 42
#define EOPNOTSUPP 95
#define EADDRINUSE 98
#define EISCONN 106
#define ENOTCONN 107

/* --- CPU time: times, getrusage ------------------------------------------- */

static void t_cputime(void)
{
    long start = now_ms();
    volatile unsigned long spin = 0;
    while (now_ms() - start < 80)       /* mostly user time: few syscalls */
        for (int k = 0; k < 200000; k++)
            spin++;
    (void)spin;
    long ru[18];                        /* struct rusage: 2 timevals + 14 longs */
    fill(ru, 0xEE, sizeof ru);
    long r = SC2(S_getrusage, 0, ru);
    long user_ms = ru[0] * 1000 + ru[1] / 1000;
    long untouched;
    fill(&untouched, 0xEE, sizeof untouched);
    check(r == 0 && user_ms >= 40 && user_ms < 10000 && ru[17] != untouched,
          "getrusage", user_ms);
    long tms[4];
    long t = SC1(S_times, tms);
    check(t > 0 && tms[0] >= 4 && tms[2] >= 0, "times", tms[0]);
}

/* --- clocks: time, gettimeofday, adjtimex --------------------------------- */

static void t_clocks(void)
{
    long tv[2];
    long r = SC2(S_gettimeofday, tv, 0);
    long t = SC1(S_time, 0);
    check(r == 0 && tv[0] > 1700000000 && (t - tv[0] < 2 && tv[0] - t < 2) &&
          tv[1] >= 0 && tv[1] < 1000000, "gettimeofday + time", tv[0]);
    /* struct timex: modes, then longs (the compiler pads it as Linux does) */
    struct { unsigned modes; long offset, freq, maxerror, esterror; int status;
             long constant, precision, tolerance, sec, usec, tick, ppsfreq, jitter;
             int shift; long stabil, jitcnt, calcnt, errcnt, stbcnt; int tai;
             int pad[11]; } tx;
    fill(&tx, 0, sizeof tx);
    r = SC1(S_adjtimex, &tx);
    check((r == 0 || r == 5) && tx.tick == 10000 && tx.sec > 1700000000, "adjtimex", r);
}

/* --- sendfile -------------------------------------------------------------- */

static char buf[12000], buf2[12000];

static void t_sendfile(void)
{
    for (int i = 0; i < (int)sizeof buf; i++)
        buf[i] = (char)('A' + i % 26);
    long src = SC3(S_open, "/tmp/sf_src", O_RDWR | O_CREAT | O_TRUNC, 0644);
    SC3(S_write, src, buf, 10000);
    long dst = SC3(S_open, "/tmp/sf_dst", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int64_t off = 100;
    long n = SC4(S_sendfile, dst, src, &off, 9000);
    SC3(S_lseek, dst, 0, 0);
    long got = SC3(S_read, dst, buf2, sizeof buf2);
    check(n == 9000 && off == 9100 && got == 9000 && same(buf2, buf + 100, 9000),
          "sendfile file->file", n);
    int p[2];
    SC1(S_pipe, p);
    SC3(S_lseek, src, 0, 0);
    n = SC4(S_sendfile, p[1], src, 0, 3000);
    long pos = SC3(S_lseek, src, 0, 1);         /* moved the file position */
    got = SC3(S_read, p[0], buf2, sizeof buf2);
    check(n == 3000 && pos == 3000 && got == 3000 && same(buf2, buf, 3000),
          "sendfile file->pipe", n);
    SC1(S_close, p[0]); SC1(S_close, p[1]); SC1(S_close, src); SC1(S_close, dst);
}

/* copy_file_range with both offsets given (the files' positions stay), and
   the extended-attribute calls' "not supported" answers. */
static void t_copy_range(void)
{
    long src = SC3(S_open, "/tmp/sf_src", O_RDONLY, 0);      /* (from t_sendfile) */
    long dst = SC3(S_open, "/tmp/cfr_dst", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int64_t in_off = 10, out_off = 5;
    long n = SC6(S_copy_file_range, src, &in_off, dst, &out_off, 4000, 0);
    long pos = SC3(S_lseek, dst, 0, 1);
    SC3(S_lseek, dst, 5, 0);
    long got = SC3(S_read, dst, buf2, 4000);
    check(n == 4000 && in_off == 4010 && out_off == 4005 && pos == 0 && got == 4000 &&
          same(buf2, buf + 10, 4000), "copy_file_range", n);
    check(SC5(S_fsetxattr, dst, "user.x", "v", 1, 0) == -95 &&
          SC3(S_flistxattr, dst, buf2, 100) == 0 &&
          SC4(S_getxattr, "/tmp/no-such-file", "user.x", buf2, 100) == -2,
          "xattr: not supported", 0);
    SC1(S_close, src);
    SC1(S_close, dst);
}

/* --- signals: sigaction with SA_SIGINFO, setitimer, pause ----------------- */

#define SA_SIGINFO  4
#define SA_RESTORER 0x04000000
#define SA_ONSTACK  0x08000000
#define SIGUSR1 10
#define SIGALRM 14
static volatile int got_sig, got_signo, got_ctx;

static void on_info(int sig, void *info, void *uc)
{
    got_sig = sig;
    got_signo = *(int *)info;           /* si_signo comes first */
    got_ctx = uc != 0;
}

static void on_alarm(int sig) { got_sig = sig; }

/* sigaltstack: the handler notes where its own stack is. */
static char altstack[16384] __attribute__((aligned(16)));
static volatile long handler_sp;

static void on_alt(int sig, void *info, void *uc)
{
    volatile char here;
    (void)info, (void)uc;
    handler_sp = (long)&here;
    got_sig = sig;
}

static long sigaction_k(int sig, void *handler, unsigned long flags)
{
    char *r = restore_rt;
#if !X64
    if (!(flags & SA_SIGINFO))
        r = restore;
#endif
    struct { long handler; unsigned long flags; long restorer; uint64_t mask; } a =
        { (long)handler, flags | SA_RESTORER, (long)r, 0 };
    return SC4(S_rt_sigaction, sig, &a, 0, 8);
}

static void t_signals(void)
{
    sigaction_k(SIGUSR1, (void *)on_info, SA_SIGINFO);
    got_sig = 0;
    SC2(S_kill, SC0(S_getpid), SIGUSR1);
    check(got_sig == SIGUSR1 && got_signo == SIGUSR1 && got_ctx, "SA_SIGINFO handler", got_sig);

    sigaction_k(SIGALRM, (void *)on_alarm, 0);
    got_sig = 0;
    long it[4] = { 0, 0, 0, 30000 };    /* struct itimerval: once, in 30 ms */
    long r = SC3(S_setitimer, 0, it, 0);
    long start = now_ms();
    long p = SC0(S_pause);
    long took = now_ms() - start;
    check(r == 0 && p == -EINTR && got_sig == SIGALRM && took >= 20 && took < 1000,
          "setitimer + pause", took);

    /* A handler with SA_ONSTACK runs on the alternate stack; afterwards the
       stack is still set, and not in use (flags 0). */
    struct { void *sp; int flags; unsigned long size; } ss = { altstack, 0, sizeof altstack }, old;
    long s1 = SC2(S_sigaltstack, &ss, 0);
    sigaction_k(SIGUSR1, (void *)on_alt, SA_SIGINFO | SA_ONSTACK);
    handler_sp = 0;
    SC2(S_kill, SC0(S_getpid), SIGUSR1);
    long s2 = SC2(S_sigaltstack, 0, &old);
    int on = handler_sp > (long)altstack && handler_sp < (long)altstack + (long)sizeof altstack;
    ss.flags = 2;                       /* SS_DISABLE */
    long s3 = SC2(S_sigaltstack, &ss, &old);
    ss.flags = 0, ss.size = 100;        /* too small */
    long s4 = SC2(S_sigaltstack, &ss, 0);
    check(s1 == 0 && s2 == 0 && on && old.flags == 0 && old.size == sizeof altstack &&
          s3 == 0 && s4 == -12, "sigaltstack + SA_ONSTACK", on ? s4 : handler_sp);

    /* A signal with our own info (SI_QUEUE) to our own thread arrives; a
       kernel-style code (SI_USER) for another process is refused. */
    int info[32] = { SIGUSR1, 0, -1 }, fake[32] = { SIGUSR1, 0, 0 };
    sigaction_k(SIGUSR1, (void *)on_info, SA_SIGINFO);
    got_sig = 0;
    long q1 = SC4(S_rt_tgsigqueueinfo, SC0(S_getpid), SC0(S_gettid), SIGUSR1, info);
    long q2 = SC3(S_rt_sigqueueinfo, 1, SIGUSR1, fake);
    check(q1 == 0 && got_sig == SIGUSR1 && q2 == -1, "rt_tgsigqueueinfo + EPERM rule", q1 ? q1 : q2);
}

/* --- System V IPC ------------------------------------------------------------ */

#if X64
static long semget_(long key, long n, long flg) { return SC3(S_semget, key, n, flg); }
static long semop_(long id, void *ops, long n) { return SC3(S_semop, id, ops, n); }
static long semctl_(long id, long num, long cmd, long arg) { return SC4(S_semctl, id, num, cmd, arg); }
static long msgget_(long key, long flg) { return SC2(S_msgget, key, flg); }
static long msgsnd_(long id, void *m, long sz, long flg) { return SC4(S_msgsnd, id, m, sz, flg); }
static long msgrcv_(long id, void *m, long sz, long type, long flg) { return SC5(S_msgrcv, id, m, sz, type, flg); }
static long msgctl_(long id, long cmd, void *b) { return SC3(S_msgctl, id, cmd, b); }
#else   /* i386: through ipc(), as musl does */
static long semget_(long key, long n, long flg) { return SC4(S_ipc, 2, key, n, flg); }
static long semop_(long id, void *ops, long n) { return SC5(S_ipc, 1, id, n, 0, ops); }
static long semctl_(long id, long num, long cmd, long arg) { return SC5(S_ipc, 3, id, num, cmd, &arg); }
static long msgget_(long key, long flg) { return SC3(S_ipc, 13, key, flg); }
static long msgsnd_(long id, void *m, long sz, long flg) { return SC5(S_ipc, 11, id, sz, flg, m); }
static long msgrcv_(long id, void *m, long sz, long type, long flg)
{
    long kludge[2] = { (long)m, type };
    return SC5(S_ipc, 12, id, sz, flg, kludge);
}
static long msgctl_(long id, long cmd, void *b) { return SC5(S_ipc, 14, id, cmd, 0, b); }
#endif

struct sembuf { unsigned short num; short op, flg; };
#define SEM_UNDO 0x1000
#define GETVAL 12
#define SETVAL 16

static void t_sem(void)
{
    long id = semget_(IPC_PRIVATE, 2, 0600);
    long r1 = semctl_(id, 0, SETVAL, 1);
    struct sembuf take = { 0, -1, IPC_NOWAIT }, give = { 0, 1, 0 };
    long a = semop_(id, &take, 1), b = semop_(id, &take, 1);
    long v0 = semctl_(id, 0, GETVAL, 0);
    semop_(id, &give, 1);
    long v1 = semctl_(id, 0, GETVAL, 0);
    check(id >= 0 && r1 == 0 && a == 0 && b == -EAGAIN && v0 == 0 && v1 == 1, "semop", b);

    /* struct semid64_ds: the perm part, then longs; nsems after 4 of them */
    long ds[32];
    fill(ds, 0, sizeof ds);
    long st = semctl_(id, 0, IPC_STAT | IPC_64, (long)ds);
    long nsems = X64 ? ds[6 + 4] : ds[9 + 4];   /* (perm: 48 / 36 bytes) */
    check(st == 0 && nsems == 2, "semctl IPC_STAT", nsems);

    long pid = SC0(S_fork);
    if (pid == 0) {                     /* take it with SEM_UNDO and die */
        struct sembuf u = { 0, -1, SEM_UNDO };
        semop_(id, &u, 1);
        child_exit(semctl_(id, 0, GETVAL, 0) == 0 ? 0 : 1);
    }
    int ws = wait_child(pid);
    long back = semctl_(id, 0, GETVAL, 0);
    check(ws == 0 && back == 1, "SEM_UNDO at exit", back);
    check(semctl_(id, 0, IPC_RMID, 0) == 0 && semctl_(id, 0, GETVAL, 0) == -EINVAL,
          "semctl IPC_RMID", 0);
}

static void t_msg(void)
{
    long id = msgget_(IPC_PRIVATE, 0600);
    struct { long type; char text[16]; } m;
    m.type = 3; m.text[0] = 'c'; msgsnd_(id, &m, 1, 0);
    m.type = 1; m.text[0] = 'a'; msgsnd_(id, &m, 1, 0);
    m.type = 2; m.text[0] = 'b'; long s = msgsnd_(id, &m, 1, 0);
    long r1 = msgrcv_(id, &m, sizeof m.text, -3, 0);       /* lowest type <= 3 */
    int ok1 = r1 == 1 && m.type == 1 && m.text[0] == 'a';
    long r2 = msgrcv_(id, &m, sizeof m.text, 0, 0);        /* the first */
    int ok2 = r2 == 1 && m.type == 3 && m.text[0] == 'c';
    long r3 = msgrcv_(id, &m, sizeof m.text, 2, 0);        /* type 2 */
    int ok3 = r3 == 1 && m.type == 2 && m.text[0] == 'b';
    long r4 = msgrcv_(id, &m, sizeof m.text, 0, IPC_NOWAIT);
    check(id >= 0 && s == 0 && ok1 && ok2 && ok3 && r4 == -ENOMSG, "msgsnd + msgrcv", r4);
    long ds[40];
    fill(ds, 0xEE, sizeof ds);
    long st = msgctl_(id, IPC_STAT | IPC_64, ds);
    /* qnum: after the perm part and 3 (x86_64) or 6 (i386) time words, + cbytes */
    long qnum = X64 ? ds[6 + 3 + 1] : ds[9 + 6 + 1];
    check(st == 0 && qnum == 0, "msgctl IPC_STAT", qnum);
    check(msgctl_(id, IPC_RMID, 0) == 0, "msgctl IPC_RMID", 0);
}

static void t_shm(void)                 /* the direct calls, on both CPUs */
{
    long id = SC3(S_shmget, IPC_PRIVATE, 4096, 0600);
    volatile char *p = (char *)SC3(S_shmat, id, 0, 0);
    p[0] = 'p';
    long pid = SC0(S_fork);
    if (pid == 0) {
        p[1] = 'c';
        child_exit(0);
    }
    wait_child(pid);
    check(id >= 0 && (long)p > 0 && p[1] == 'c', "shmget/shmat direct", (long)p);
    check(SC1(S_shmdt, p) == 0 && SC3(S_shmctl, id, IPC_RMID, 0) == 0, "shmdt + shmctl", 0);
}

/* --- the direction flag ------------------------------------------------------ */

/* A program may have the direction flag set (std) at the moment an
   interrupt or a fault takes it into the kernel — musl's memmove does, for
   a moment. The kernel must clear it for itself: with DF set, its own
   memset and memcpy run backwards, over whatever lies below their target.
   (The x86_64 kernel did not, until 2026-10-08: the console's grid was
   once copied back over the whole kernel image.) Here: dirty some pages
   and give them back, so a recycled frame is not zero by luck; then, with
   DF set, the first touch of each fresh page — a page fault, whose handler
   zero-fills the new frame. Every page must read back as zeros. */
#define PROT_RW       3
#define MAP_PRIV_ANON 0x22
static void t_dirflag(void)
{
    const long n = 64, len = 64 * 4096;
    char *dirty = (char *)SC6(S_mmap, 0, len, PROT_RW, MAP_PRIV_ANON, -1, 0);
    char *p = (char *)SC6(S_mmap, 0, len, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if ((unsigned long)dirty > -4096UL || (unsigned long)p > -4096UL) {
        check(0, "direction flag: mmap", (long)p);
        return;
    }
    fill(dirty, 0xA5, len);
    SC2(S_munmap, dirty, len);          /* its frames go back, dirty */
    long bad = 0;
    for (long k = 0; k < n; k++) {
        volatile char *page = p + k * 4096;
        __asm__ volatile ("std\n\tmovb $1, (%0)\n\tcld" : : "r"(page) : "memory");
        for (int b = 1; b < 4096; b++)
            if (page[b]) {
                bad++;
                break;
            }
    }
    check(bad == 0, "direction flag: a page fault while DF is set zero-fills the page", bad);
    SC2(S_munmap, p, len);
}

/* --- inotify ----------------------------------------------------------------- */

#define IN_CREATE 0x100
#define IN_DELETE 0x200

static void t_inotify(void)
{
    SC2(S_mkdir, "/tmp/itest", 0755);
    long fd = SC1(S_inotify_init1, 0);
    long wd = SC3(S_inotify_add_watch, fd, "/tmp/itest", IN_CREATE | IN_DELETE);
    long f = SC3(S_open, "/tmp/itest/f", O_RDWR | O_CREAT, 0644);
    SC1(S_close, f);
    SC1(S_unlink, "/tmp/itest/f");
    char ev[256];
    long n = SC3(S_read, fd, ev, sizeof ev);
    /* struct inotify_event: wd, mask, cookie, len, name (len bytes) */
    int32_t *e1 = (int32_t *)ev;
    int32_t *e2 = (int32_t *)(ev + 16 + e1[3]);
    check(n == 2 * (16 + 16) && e1[0] == wd && e1[1] == IN_CREATE && e1[3] == 16 &&
          ev[16] == 'f' && e2[1] == IN_DELETE, "inotify", n);
    check(SC2(S_inotify_rm_watch, fd, wd) == 0, "inotify_rm_watch", 0);
    SC1(S_close, fd);
}

/* --- fcntl record locks ------------------------------------------------------- */

struct flock_ { short type, whence; int64_t start, len; int pid; };
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define F_OFD_SETLK 37

static void t_locks(void)
{
    long fd = SC3(S_open, "/tmp/lockf", O_RDWR | O_CREAT, 0644);
    struct flock_ l = { F_WRLCK, 0, 0, 100, 0 };
    long r = SC3(S_fcntl, fd, F_SETLK, &l);
    long me = SC0(S_getpid);
    long pid = SC0(S_fork);
    if (pid == 0) {
        long cfd = SC3(S_open, "/tmp/lockf", O_RDWR, 0);
        struct flock_ q = { F_RDLCK, 0, 50, 10, 0 };
        SC3(S_fcntl, cfd, F_GETLK, &q);
        int ok = q.type == F_WRLCK && q.pid == me && q.start == 0 && q.len == 100;
        struct flock_ w = { F_RDLCK, 0, 50, 10, 0 };
        long e = SC3(S_fcntl, cfd, F_SETLK, &w);       /* held: no */
        struct flock_ w2 = { F_RDLCK, 0, 100, 10, 0 };
        long e2 = SC3(S_fcntl, cfd, F_SETLK, &w2);     /* past it: yes */
        child_exit(ok && e == -EAGAIN && e2 == 0 ? 0 : 1);
    }
    int ws = wait_child(pid);
    check(r == 0 && ws == 0, "fcntl F_SETLK/F_GETLK", ws);
    long fd2 = SC3(S_open, "/tmp/lockf", O_RDWR, 0);  /* another open file */
    struct flock_ o = { F_WRLCK, 0, 0, 10, 0 };
    long e = SC3(S_fcntl, fd2, F_OFD_SETLK, &o);       /* our POSIX lock is in the way */
    struct flock_ u = { F_UNLCK, 0, 0, 0, 0 };
    SC3(S_fcntl, fd, F_SETLK, &u);
    long e2 = SC3(S_fcntl, fd2, F_OFD_SETLK, &o);
    check(e == -EAGAIN && e2 == 0, "OFD locks", e);
    SC1(S_close, fd2);
    SC1(S_close, fd);
}

/* --- the rest ----------------------------------------------------------------- */

static void t_misc(void)
{
    /* chroot, in a child: ".." can't get out, and what it makes is in the jail */
    SC2(S_mkdir, "/tmp/jail", 0755);
    long pid = SC0(S_fork);
    if (pid == 0) {
        long r = SC1(S_chroot, "/tmp/jail");
        SC1(S_chdir, "/");
        SC1(S_chdir, "../../..");
        char cwd[64];
        long n = SC2(S_getcwd, cwd, sizeof cwd);
        SC2(S_mkdir, "/made", 0755);
        child_exit(r == 0 && n == 2 && cwd[0] == '/' && cwd[1] == 0 ? 0 : 1);
    }
    int ws = wait_child(pid);
    long there = SC2(S_rmdir, "/tmp/jail/made", 0);
    check(ws == 0 && there == 0, "chroot", ws);

    long old = SC1(S_personality, 0xffffffff);
#if X64
    SC1(S_personality, 8);              /* PER_LINUX32 */
    char u[6][65];
    SC1(S_uname, u);
    int i686 = same(u[4], "i686", 5);
    SC1(S_personality, 0);
    check(old == 0 && i686, "personality PER_LINUX32", old);
#else
    check(old == 0, "personality", old);
#endif

    uint32_t hdr[2] = { 0x20080522, 0 }, data[6];
    long c = SC2(S_capget, hdr, data);
    check(c == 0 && (data[0] & 1) && data[1] == data[0], "capget", c);

    long size = SC3(S_syslog, 10, 0, 0);
    long n = SC3(S_syslog, 3, buf, sizeof buf);
    check(size == 65536 && n > 0 && contains(buf, n, "MyOS"), "syslog (dmesg)", n);

    long sf[32];
    fill(sf, 0, sizeof sf);
#if X64
    long st = SC2(S_statfs, "/", sf);
    long bsize = sf[1];
#else
    long st = SC3(S_statfs, "/", 84, sf);   /* statfs64 */
    long bsize = sf[1];
#endif
    check(st == 0 && bsize == 4096, "statfs", bsize);

    long si[32];
    fill(si, 0, sizeof si);
    long sr = SC1(S_sysinfo, si);
    check(sr == 0 && si[0] >= 0 && si[4] > 0, "sysinfo", si[4]);

    long tv[2] = { 0, 20000 };
    long start = now_ms();
    long sel = SC5(S_select, 0, 0, 0, 0, tv);
    long took = now_ms() - start;
    check(sel == 0 && took >= 15 && took < 1000, "select timeout", took);

    check(SC1(S_unshare, 0) == 0 && SC1(S_unshare, 0x40000000) == -EINVAL, "unshare", 0);
    long fd = SC3(S_open, "/tmp/sf_src", O_RDONLY, 0);
    check(SC1(S_syncfs, fd) == 0, "syncfs", 0);
#if X64
    check(SC4(S_fadvise, fd, 0, 0, 2) == 0, "fadvise", 0);
#else
    check(SC6(S_fadvise, fd, 0, 0, 0, 0, 2) == 0, "fadvise", 0);
#endif
    SC1(S_close, fd);
}

/* --- netlink and the interface ioctls ---------------------------------- */

struct nlmsghdr { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nlattr { uint16_t len, type; };
struct sockaddr_nl { uint16_t family, pad; uint32_t pid, groups; };
#define AF_UNIX 1
#define AF_NETLINK 16
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define NETLINK_ROUTE 0
#define NETLINK_GENERIC 16
#define NLM_F_REQUEST 1
#define NLM_F_DUMP 0x300
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define GENL_ID_CTRL 0x10
#define ENOENT 2

/* Send a request and read one answer datagram into buf: its length. */
static long nl_ask(long fd, void *req, uint32_t len, void *buf, uint32_t size)
{
    struct sockaddr_nl k = { AF_NETLINK, 0, 0, 0 };
    if (SC6(S_sendto, fd, req, len, 0, &k, sizeof k) != (long)len)
        return -1;
    return SC6(S_recvfrom, fd, buf, size, 0x40 /* MSG_DONTWAIT */, 0, 0);
}

/* The first attribute `type` after a fixed header of `skip` bytes. */
static const struct nlattr *nl_attr(const struct nlmsghdr *h, uint32_t skip, uint16_t type)
{
    const uint8_t *p = (const uint8_t *)(h + 1) + skip, *end = (const uint8_t *)h + h->len;
    while (p + 4 <= end) {
        const struct nlattr *a = (const void *)p;
        if (a->len < 4)
            break;
        if ((a->type & 0x3FFF) == type)
            return a;
        p += (a->len + 3u) & ~3u;
    }
    return 0;
}

static void t_netlink(void)
{
    static uint32_t buf[1024];          /* 4 KiB, aligned */
    long fd = SC3(S_socket, AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    struct sockaddr_nl me = { AF_NETLINK, 0, 0, 0 };
    uint32_t mlen = sizeof me;
    check(fd >= 0 && SC3(S_bind, fd, &me, sizeof me) == 0 &&
          SC3(S_getsockname, fd, &me, &mlen) == 0 && mlen == sizeof me &&
          me.family == AF_NETLINK && me.pid != 0, "netlink socket, bind", fd);

    /* RTM_GETLINK dump: lo is link 1, then NLMSG_DONE. */
    struct { struct nlmsghdr h; uint8_t family, pad[3]; } dump =
        { { sizeof dump, RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 7, 0 }, 0, { 0 } };
    long n = nl_ask(fd, &dump, sizeof dump, buf, sizeof buf);
    int lo = 0, done = 0, seq_ok = 1;
    for (long off = 0; n > 0 && off + 16 <= n; ) {
        const struct nlmsghdr *h = (const void *)((uint8_t *)buf + off);
        if (h->len < 16 || off + (long)h->len > n)
            break;
        seq_ok &= h->seq == 7 && h->pid == me.pid;
        if (h->type == RTM_NEWLINK) {
            const struct nlattr *a = nl_attr(h, 16, 3 /* IFLA_IFNAME */);
            int32_t index = *(const int32_t *)((const uint8_t *)(h + 1) + 4);
            if (a && same(a + 1, "lo", 3) && index == 1)
                lo = 1;
        }
        if (h->type == NLMSG_DONE)
            done = 1;
        off += (h->len + 3u) & ~3u;
    }
    check(lo && done && seq_ok, "rtnetlink link dump", n);
    SC1(S_close, fd);

    /* Generic netlink: the controller knows itself; nl80211 isn't there yet. */
    fd = SC3(S_socket, AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    struct {
        struct nlmsghdr h;
        uint8_t cmd, version; uint16_t reserved;
        struct nlattr a; char name[8];
    } q = { { sizeof q, GENL_ID_CTRL, NLM_F_REQUEST, 1, 0 }, 3 /* GETFAMILY */, 1, 0,
            { 4 + 7, 2 /* CTRL_ATTR_FAMILY_NAME */ }, "nlctrl" };
    n = nl_ask(fd, &q, sizeof q, buf, sizeof buf);
    const struct nlmsghdr *h = (const void *)buf;
    const struct nlattr *id = n > 16 && h->type == GENL_ID_CTRL ? nl_attr(h, 4, 1) : 0;
    check(id && *(const uint16_t *)(id + 1) == GENL_ID_CTRL, "genetlink family nlctrl", n);
    q.a.len = 4 + 7;
    for (int k = 0; k < 7; k++)
        q.name[k] = "nl8021"[k];
    n = nl_ask(fd, &q, sizeof q, buf, sizeof buf);
    check(n >= 20 && h->type == NLMSG_ERROR && (int32_t)buf[4] == -ENOENT,
          "genetlink unknown family", n >= 20 ? (int32_t)buf[4] : n);
    SC1(S_close, fd);

    /* SIOCGIFINDEX on a UNIX socket, as musl's if_nametoindex asks. */
    struct { char name[16]; int32_t v; uint8_t pad[2 * sizeof(void *) + 4]; } ifr;
    fill(&ifr, 0, sizeof ifr);
    ifr.name[0] = 'l';
    ifr.name[1] = 'o';
    fd = SC3(S_socket, AF_UNIX, SOCK_DGRAM, 0);
    check(SC3(S_ioctl, fd, 0x8933 /* SIOCGIFINDEX */, &ifr) == 0 && ifr.v == 1,
          "SIOCGIFINDEX lo", ifr.v);
    SC1(S_close, fd);
}

/* --- AF_PACKET: link-layer frames (wpa_supplicant's EAPOL, DHCP clients) --- */

struct sockaddr_ll { uint16_t family, protocol; int32_t ifindex; uint16_t hatype;
                     uint8_t pkttype, halen, addr[8]; };

/* (Out of line: on i386 a six-argument syscall needs every register.) */
static __attribute__((noinline)) long p_sendto(long fd, const void *b, long n, const void *to, long tl)
{
    return SC6(S_sendto, fd, b, n, 0, to, tl);
}
static __attribute__((noinline)) long p_recvfrom(long fd, void *b, long n, void *from, uint32_t *fl)
{
    return SC6(S_recvfrom, fd, b, n, 0x40 /* MSG_DONTWAIT */, from, fl);
}

static void t_packet(void)
{
    /* eth0's index and address, as wpa_supplicant's l2_packet asks for them. */
    struct { char name[16]; union { int32_t index; struct { uint16_t family; uint8_t mac[14]; } hw; } u;
             uint8_t pad[2 * sizeof(void *)]; } ifr;
    long fd = SC3(S_socket, 17 /* AF_PACKET */, SOCK_DGRAM, 0x0608 /* htons(ETH_P_ARP) */);
    check(fd >= 0, "AF_PACKET socket", fd);
    fill(&ifr, 0, sizeof ifr);
    for (int k = 0; k < 5; k++)
        ifr.name[k] = "eth0"[k];
    if (SC3(S_ioctl, fd, 0x8933 /* SIOCGIFINDEX */, &ifr) != 0) {
        SC1(S_close, fd);
        return;                         /* no eth0 here */
    }
    int32_t idx = ifr.u.index;
    check(SC3(S_ioctl, fd, 0x8927 /* SIOCGIFHWADDR */, &ifr) == 0, "packet socket: SIOCGIFHWADDR", 0);
    uint8_t mac[6];
    for (int k = 0; k < 6; k++)
        mac[k] = ifr.u.hw.mac[k];
    struct sockaddr_ll ll = { 17, 0x0608, idx, 1, 0, 6, { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
    check(SC3(S_bind, fd, &ll, sizeof ll) == 0, "packet socket: bind to eth0, ARP", 0);

    /* Who has 10.0.2.2 (QEMU's gateway)? Sent and answered through the socket. */
    uint8_t arp[28] = { 0, 1, 8, 0, 6, 4, 0, 1 };
    for (int k = 0; k < 6; k++)
        arp[8 + k] = mac[k];
    arp[14] = 10; arp[15] = 0; arp[16] = 2; arp[17] = 15;
    arp[24] = 10; arp[25] = 0; arp[26] = 2; arp[27] = 2;
    check(p_sendto(fd, arp, sizeof arp, &ll, sizeof ll) == (long)sizeof arp,
          "packet socket: send an ARP request", 0);
    static uint8_t got[64];
    struct sockaddr_ll from;
    uint32_t flen = sizeof from;
    long n = 0, start = now_ms();
    while (now_ms() - start < 2000) {
        flen = sizeof from;
        n = p_recvfrom(fd, got, sizeof got, &from, &flen);
        if (n >= 28 && got[7] == 2 && got[14] == 10 && got[17] == 2)
            break;                      /* the reply (not our own request) */
        long tv[2] = { 0, 10000 };
        SC5(S_select, 0, 0, 0, 0, tv);
    }
    check(n >= 28 && got[7] == 2 && from.family == 17 && from.ifindex == idx && from.halen == 6 &&
          same(from.addr, got + 8, 6), "packet socket: the ARP reply, with the sender's address", n);
    SC1(S_close, fd);
}

/* --- nl80211: Wi-Fi's netlink family (W2c) ------------------------------ */

#define NL80211_CMD_GET_INTERFACE    5
#define NL80211_CMD_NEW_INTERFACE    7
#define NL80211_CMD_GET_SCAN         32
#define NL80211_CMD_TRIGGER_SCAN     33
#define NL80211_CMD_NEW_SCAN_RESULTS 34
#define NL80211_ATTR_IFINDEX         3
#define NL80211_ATTR_IFNAME          4
#define NL80211_ATTR_BSS             47
#define NL80211_BSS_FREQUENCY        2
#define NL80211_BSS_INFORMATION_ELEMENTS 6
#define NL80211_BSS_SIGNAL_MBM       7
#define NLM_F_ACK                    4

struct genl_req {
    struct nlmsghdr h;
    uint8_t cmd, version; uint16_t reserved;
    struct nlattr a; uint32_t v;        /* one u32 attribute (the ifindex) */
};

/* Every message in a datagram of n bytes: f(message) for each. */
#define each_msg(h, base, n) \
    for (const struct nlmsghdr *h = (const void *)(base); \
         (const uint8_t *)h + 16 <= (const uint8_t *)(base) + (n) && h->len >= 16 && \
         (const uint8_t *)h + h->len <= (const uint8_t *)(base) + (n); \
         h = (const void *)((const uint8_t *)h + ((h->len + 3u) & ~3u)))

/* The attribute `type` inside a nested attribute. */
static const struct nlattr *nested(const struct nlattr *outer, uint16_t type)
{
    const uint8_t *p = (const uint8_t *)(outer + 1), *end = (const uint8_t *)outer + outer->len;
    while (p + 4 <= end) {
        const struct nlattr *a = (const void *)p;
        if (a->len < 4 || p + a->len > end)
            break;
        if ((a->type & 0x3FFF) == type)
            return a;
        p += (a->len + 3u) & ~3u;
    }
    return 0;
}

/* The family "nl80211": its id, and the id of its multicast group "scan". */
static int nl80211_ids(long fd, uint16_t *family, uint32_t *scan, uint32_t *buf, uint32_t size)
{
    struct {
        struct nlmsghdr h;
        uint8_t cmd, version; uint16_t reserved;
        struct nlattr a; char name[8];
    } q = { { sizeof q, GENL_ID_CTRL, NLM_F_REQUEST, 1, 0 }, 3, 1, 0,
            { 4 + 8, 2 }, "nl80211" };
    long n = nl_ask(fd, &q, sizeof q, buf, size);
    const struct nlmsghdr *h = (const void *)buf;
    if (n < 20 || h->type != GENL_ID_CTRL)
        return 0;
    const struct nlattr *id = nl_attr(h, 4, 1), *groups = nl_attr(h, 4, 7);
    if (!id || !groups)
        return 0;
    *family = *(const uint16_t *)(id + 1);
    for (uint16_t k = 1; k < 16; k++) {
        const struct nlattr *g = nested(groups, k);
        const struct nlattr *name = g ? nested(g, 1) : 0, *gid = g ? nested(g, 2) : 0;
        if (name && gid && same(name + 1, "scan", 5))
            *scan = *(const uint32_t *)(gid + 1);
    }
    return *scan != 0;
}

/* Up to `ms` milliseconds for a datagram: its length, or 0. */
static long nl_wait(long fd, void *buf, uint32_t size, long ms)
{
    long start = now_ms();
    while (now_ms() - start < ms) {
        long n = SC6(S_recvfrom, fd, buf, size, 0x40 /* MSG_DONTWAIT */, 0, 0);
        if (n > 0)
            return n;
        long tv[2] = { 0, 10000 };
        SC5(S_select, 0, 0, 0, 0, tv);
    }
    return 0;
}

static void t_nl80211(void)
{
    static uint32_t buf[2048];          /* 8 KiB, aligned */
    long fd = SC3(S_socket, AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    uint16_t fam = 0;
    uint32_t scan_grp = 0;
    check(nl80211_ids(fd, &fam, &scan_grp, buf, sizeof buf), "nl80211 family, its scan group",
          fam);

    /* The interfaces: is there a wlan0? (Only `make test` kernels have a radio.) */
    struct genl_req r = { { 20, fam, NLM_F_REQUEST | NLM_F_DUMP, 2, 0 },
                          NL80211_CMD_GET_INTERFACE, 1, 0, { 0, 0 }, 0 };
    long n = nl_ask(fd, &r, 20, buf, sizeof buf);
    uint32_t ifindex = 0;
    each_msg(h, buf, n) {
        const struct nlattr *name = h->type == fam ? nl_attr(h, 4, NL80211_ATTR_IFNAME) : 0;
        const struct nlattr *idx = h->type == fam ? nl_attr(h, 4, NL80211_ATTR_IFINDEX) : 0;
        if (name && idx && same(name + 1, "wlan0", 6))
            ifindex = *(const uint32_t *)(idx + 1);
    }
    if (!ifindex) {
        SC1(S_close, fd);
        return;                         /* no Wi-Fi radio in this kernel */
    }
    check(ifindex > 1, "nl80211 interface dump: wlan0", (long)ifindex);

    /* Up, as `ip link set wlan0 up` does it. */
    struct { char name[16]; int16_t flags; uint8_t pad[2 * sizeof(void *) + 6]; } ifr;
    fill(&ifr, 0, sizeof ifr);
    for (int k = 0; k < 6; k++)
        ifr.name[k] = "wlan0"[k];
    long ufd = SC3(S_socket, AF_UNIX, SOCK_DGRAM, 0);
    SC3(S_ioctl, ufd, 0x8913 /* SIOCGIFFLAGS */, &ifr);
    ifr.flags |= 1;                     /* IFF_UP */
    check(SC3(S_ioctl, ufd, 0x8914 /* SIOCSIFFLAGS */, &ifr) == 0, "wlan0 up", 0);

    /* Listen for scan events, start a scan, wait for the results event. */
    check(SC5(S_setsockopt, fd, 270 /* SOL_NETLINK */, 1 /* ADD_MEMBERSHIP */, &scan_grp, 4) == 0,
          "join the scan group", (long)scan_grp);
    struct genl_req t = { { sizeof t, fam, NLM_F_REQUEST | NLM_F_ACK, 3, 0 },
                          NL80211_CMD_TRIGGER_SCAN, 1, 0, { 8, NL80211_ATTR_IFINDEX }, ifindex };
    struct sockaddr_nl k = { AF_NETLINK, 0, 0, 0 };
    SC6(S_sendto, fd, &t, sizeof t, 0, &k, sizeof k);
    int acked = 0, started = 0, results = 0;
    long err = 1;
    for (int tries = 0; tries < 8 && !results; tries++) {
        n = nl_wait(fd, buf, sizeof buf, 3000);
        each_msg(h, buf, n) {
            if (h->type == NLMSG_ERROR && h->seq == 3) {
                err = *(const int32_t *)(h + 1);
                acked = 1;
            }
            if (h->type == fam && h->seq == 0) {
                uint8_t cmd = *(const uint8_t *)(h + 1);
                started |= cmd == NL80211_CMD_TRIGGER_SCAN;
                results |= cmd == NL80211_CMD_NEW_SCAN_RESULTS;
            }
        }
    }
    check(acked && err == 0, "nl80211 TRIGGER_SCAN accepted", err);
    check(started && results, "nl80211 scan events: started, results", started * 2 + results);

    /* The results: MyOS-Test on 2437 MHz at -42 dBm. */
    struct genl_req g = { { sizeof g, fam, NLM_F_REQUEST | NLM_F_DUMP, 4, 0 },
                          NL80211_CMD_GET_SCAN, 1, 0, { 8, NL80211_ATTR_IFINDEX }, ifindex };
    n = nl_ask(fd, &g, sizeof g, buf, sizeof buf);
    int found = 0, networks = 0;
    each_msg(h, buf, n) {
        const struct nlattr *bss = h->type == fam ? nl_attr(h, 4, NL80211_ATTR_BSS) : 0;
        if (!bss)
            continue;
        networks++;
        const struct nlattr *freq = nested(bss, NL80211_BSS_FREQUENCY);
        const struct nlattr *sig = nested(bss, NL80211_BSS_SIGNAL_MBM);
        const struct nlattr *ies = nested(bss, NL80211_BSS_INFORMATION_ELEMENTS);
        const uint8_t *ie = ies ? (const uint8_t *)(ies + 1) : 0;
        if (freq && sig && ie && ie[0] == 0 /* SSID */ && ie[1] == 9 && same(ie + 2, "MyOS-Test", 9))
            found = *(const uint32_t *)(freq + 1) == 2437 && *(const int32_t *)(sig + 1) == -4200;
    }
    check(networks == 2 && found, "nl80211 scan results: MyOS-Test, 2437 MHz, -42 dBm", networks);

    ifr.flags &= ~1;
    SC3(S_ioctl, ufd, 0x8914, &ifr);
    SC1(S_close, ufd);
    SC1(S_close, fd);
}

/* --- TCP listen and accept ------------------------------------------------ */

/* What needs no peer: this may run with no network at all (test-uefi-shell
   has -net none). A real connection coming in is make test's kernel
   self-test (kernel/net_test.c) and make test-ssh. */
struct sockaddr_in4 { uint16_t family, port; uint32_t addr; uint8_t zero[8]; };
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_NONBLOCK 04000
static uint16_t be16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

static void t_tcp_listen(void)
{
    struct sockaddr_in4 a = { AF_INET, be16(7777), 0, { 0 } }, got;
    uint32_t len = sizeof got;
    long l = SC3(S_socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    long b = SC3(S_bind, l, &a, sizeof a);
    check(l >= 0 && b == 0 && SC2(S_listen, l, 4) == 0, "tcp: bind and listen", b);
    fill(&got, 0, sizeof got);
    long r = SC3(S_getsockname, l, &got, &len);
    check(r == 0 && got.family == AF_INET && got.port == be16(7777) && got.addr == 0 && len == 16,
          "tcp: getsockname on a listener (0.0.0.0:7777)", be16(got.port));
    r = SC4(S_accept4, l, 0, 0, 0);
    check(r == -EAGAIN, "tcp: accept4 with nobody waiting", r);
    check(SC4(S_accept4, l, 0, 0, 1) == -EINVAL, "tcp: accept4 with an unknown flag", 0);
    struct { int32_t fd; int16_t events, revents; } p = { (int32_t)l, 1 /* POLLIN */, 0 };
    r = SC3(S_poll, &p, 1, 0);
    check(r == 0 && p.revents == 0, "tcp: an empty listener is not readable", p.revents);
    char buf[4];
    r = SC3(S_read, l, buf, sizeof buf);
    check(r == -ENOTCONN, "tcp: read on a listener", r);
    struct sockaddr_in4 far = { AF_INET, be16(7), 0x010200C0 /* 192.0.2.1 */, { 0 } };
    r = SC3(S_connect, l, &far, sizeof far);
    check(r == -EISCONN, "tcp: connect on a listener", r);

    /* Only one listener per port: Linux refuses the bind even with
       SO_REUSEADDR (which only lets a port be reused past old connections). */
    long l2 = SC3(S_socket, AF_INET, SOCK_STREAM, 0);
    b = SC3(S_bind, l2, &a, sizeof a);
    check(b == -EADDRINUSE, "tcp: one listener per port", b);
    SC1(S_close, l2);
    /* Two may bind it while neither listens; then the second listen() loses. */
    struct sockaddr_in4 a8 = { AF_INET, be16(7778), 0, { 0 } };
    long m1 = SC3(S_socket, AF_INET, SOCK_STREAM, 0), m2 = SC3(S_socket, AF_INET, SOCK_STREAM, 0);
    b = SC3(S_bind, m1, &a8, sizeof a8) | SC3(S_bind, m2, &a8, sizeof a8);
    r = SC2(S_listen, m1, 1);
    long r2 = SC2(S_listen, m2, 1);
    check(b == 0 && r == 0 && r2 == -EADDRINUSE, "tcp: two bound, one listens", r2);
    SC1(S_close, m1);
    SC1(S_close, m2);

    long l3 = SC3(S_socket, AF_INET, SOCK_STREAM, 0);
    fill(&got, 0, sizeof got);
    len = sizeof got;
    r = SC2(S_listen, l3, 1);
    check(r == 0 && SC3(S_getsockname, l3, &got, &len) == 0 && got.port != 0,
          "tcp: listen without bind picks a port", r);
    SC1(S_close, l3);

    long u = SC3(S_socket, AF_INET, SOCK_DGRAM, 0);
    r = SC2(S_listen, u, 1);
    check(r == -EOPNOTSUPP, "udp: listen", r);
    SC1(S_close, u);

    SC1(S_close, l);                    /* the port is free again */
    long l4 = SC3(S_socket, AF_INET, SOCK_STREAM, 0);
    b = SC3(S_bind, l4, &a, sizeof a);
    check(b == 0 && SC2(S_listen, l4, 1) == 0, "tcp: a closed listener's port is free", b);
    SC1(S_close, l4);

    long pid = SC0(S_fork);
    if (pid == 0) {                     /* ports below 1024 are root's */
        struct sockaddr_in4 ssh = { AF_INET, be16(22), 0, { 0 } };
        SC1(S_setuid, 1000);
        long s = SC3(S_socket, AF_INET, SOCK_STREAM, 0);
        child_exit(SC3(S_bind, s, &ssh, sizeof ssh) == -EACCES ? 0 : 1);
    }
    check(wait_child(pid) == 0, "tcp: a user may not bind port 22", 0);
}

/* --- who may signal whom; reboot and the host name are root's ------------ */

#define SIGKILL 9
#define SIGTERM 15
static void t_perm(void)
{
    long root = SC0(S_fork);
    if (root == 0)                      /* root's process, waiting to be signalled */
        for (;;)
            SC0(S_pause);
    long pid = SC0(S_fork);
    if (pid == 0) {
        SC1(S_setuid, 1000);            /* real, effective and saved: 1000 */
        int ok = SC2(S_kill, root, 0) == -EPERM &&          /* "may I?": no */
                 SC2(S_kill, root, SIGTERM) == -EPERM &&
                 SC3(S_tgkill, root, root, SIGTERM) == -EPERM &&
                 SC2(S_kill, -1, 0) == -ESRCH &&            /* nobody we may signal */
                 SC2(S_kill, SC0(S_getpid), 0) == 0 &&      /* ourselves: yes */
                 /* Ctrl-Alt-Del off: harmless even if the check were missing */
                 SC4(S_reboot, 0xfee1dead, 672274793, 0, 0) == -EPERM &&
                 SC2(S_sethostname, "x", 1) == -EPERM;
        child_exit(ok ? 0 : 1);
    }
    check(wait_child(pid) == 0, "a user may not signal root's process, reboot, or name the host", 0);
    check(SC2(S_kill, root, SIGKILL) == 0, "root may signal anyone", 0);
    wait_child(root);
}

/* --- file permissions: Linux's rules, as user 1000 ------------------------- */

#define EEXIST 17
#define AT_FDCWD (-100)
#define AT_EACCESS 0x200
#define R_OK 4
#define W_OK 2
#define X_OK 1

static void mkfile(const char *path, int mode, int uid)
{
    long fd = SC3(S_open, path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    SC3(S_write, fd, "data", 4);
    SC1(S_close, fd);
    SC2(S_chmod, path, mode);
    SC3(S_chown, path, uid, uid);
}

/* Become user 1000 the way login does: no supplementary groups left
   from root (they would still count), group 1000, then the user. */
static void become_user(void)
{
    SC2(S_setgroups, 0, 0);
    SC1(S_setgid, 1000);
    SC1(S_setuid, 1000);
}

/* One rule per bit of `ok`, so a failure says which (check's detail). */
static long as_user(void)
{
    long bad = 0, bit = 1, fd;
#define RULE(cond) do { if (!(cond)) bad |= bit; bit <<= 1; } while (0)
    RULE(SC3(S_open, "/tmp/pt/secret", O_RDONLY, 0) == -EACCES);   /* 0600 root */
    RULE((fd = SC3(S_open, "/tmp/pt/pub", O_RDONLY, 0)) >= 0);     /* 0644 root */
    SC1(S_close, fd);
    RULE(SC3(S_open, "/tmp/pt/pub", O_WRONLY, 0) == -EACCES);
    RULE(SC3(S_open, "/tmp/pt/pub", O_RDONLY | O_TRUNC, 0) == -EACCES);
    RULE(SC3(S_open, "/tmp/pt/closed/x", O_RDONLY, 0) == -EACCES); /* dir 0700 root */
    RULE(SC2(S_access, "/tmp/pt/closed/x", 0) == -EACCES);        /* even "exists?" */
    RULE(SC1(S_chdir, "/tmp/pt/closed") == -EACCES);
    RULE(SC2(S_mkdir, "/tmp/pt/new", 0755) == -EACCES);           /* dir 0755 root */
    RULE(SC2(S_mkdir, "/tmp/pt/pub", 0755) == -EEXIST);           /* EEXIST first */
    RULE(SC1(S_unlink, "/tmp/pt/pub") == -EACCES);
    RULE(SC2(S_rename, "/tmp/pt/pub", "/tmp/pt-moved") == -EACCES);
    RULE(SC1(S_unlink, "/tmp/pt-sticky") == -EPERM);              /* root's, in /tmp */
    RULE((fd = SC3(S_open, "/tmp/pt-mine", O_WRONLY | O_CREAT, 0444)) >= 0);
    SC1(S_close, fd);                                             /* made: opened anyway */
    RULE(SC3(S_open, "/tmp/pt-mine", O_WRONLY, 0) == -EACCES);    /* now 0444 says no */
    RULE(SC2(S_chmod, "/tmp/pt-mine", 0644) == 0);                /* own file: may */
    RULE(SC3(S_chown, "/tmp/pt-mine", 0, -1) == -EPERM);          /* give it away: no */
    RULE(SC3(S_chown, "/tmp/pt-mine", -1, 0) == -EPERM);          /* not in group 0 */
    RULE(SC3(S_chown, "/tmp/pt-mine", 1000, 1000) == 0);          /* no change: fine */
    RULE(SC1(S_unlink, "/tmp/pt-mine") == 0);                     /* own, in /tmp */
    RULE(SC2(S_chmod, "/tmp/pt/pub", 0666) == -EPERM);
    RULE(SC2(S_truncate, "/tmp/pt/pub", 0) == -EACCES);
    RULE(SC4(S_utimensat, AT_FDCWD, "/tmp/pt/pub", 0, 0) == -EACCES);  /* touch */
    long when[4] = { 1, 0, 1, 0 };
    RULE(SC4(S_utimensat, AT_FDCWD, "/tmp/pt/open", when, 0) == -EPERM); /* 0666: write */
    RULE(SC4(S_utimensat, AT_FDCWD, "/tmp/pt/open", 0, 0) == 0);  /* but "now" is fine */
    RULE(SC3(S_open, "/tmp/pt/nogroup", O_RDONLY, 0) == -EACCES); /* 0044, ours: owner */
    RULE(SC2(S_access, "/tmp/pt/pub", R_OK) == 0);                /*   bits decide */
    RULE(SC2(S_access, "/tmp/pt/pub", W_OK) == -EACCES);
    RULE(SC2(S_access, "/tmp/pt/pub", X_OK) == -EACCES);
#undef RULE
    return bad;
}

static void t_fileperm(void)
{
    SC2(S_mkdir, "/tmp/pt", 0755);
    SC2(S_mkdir, "/tmp/pt/closed", 0700);
    mkfile("/tmp/pt/closed/x", 0644, 0);
    mkfile("/tmp/pt/secret", 0600, 0);
    mkfile("/tmp/pt/pub", 0644, 0);
    mkfile("/tmp/pt/open", 0666, 0);
    mkfile("/tmp/pt/nogroup", 0044, 1000);
    mkfile("/tmp/pt-sticky", 0666, 0);
    long pid = SC0(S_fork);
    if (pid == 0) {
        become_user();
        child_exit((int)as_user() ? 1 : 0);
    }
    int st = wait_child(pid);
    long detail = 0;
    if (st) {                           /* run it again here to say which */
        pid = SC0(S_fork);
        if (pid == 0) {
            become_user();
            long bad = as_user();
            long n = 0;
            while (!(bad & 1)) { bad >>= 1; n++; }
            child_exit((int)n + 1);
        }
        detail = (wait_child(pid) >> 8) & 0xFF;
    }
    check(st == 0, "file permissions as user 1000 (detail: first failed rule + 1)", detail);

    /* access() answers for the REAL user: a setuid-root program (real 1000,
       effective 0) may open the secret, but access() says its caller may not;
       AT_EACCESS asks for the effective user instead. */
    pid = SC0(S_fork);
    if (pid == 0) {
        SC3(S_setresuid, 1000, 0, 0);
        long fd = SC3(S_open, "/tmp/pt/secret", O_RDONLY, 0);
        int ok = fd >= 0 && SC2(S_access, "/tmp/pt/secret", R_OK) == -EACCES &&
                 SC4(S_faccessat2, AT_FDCWD, "/tmp/pt/secret", R_OK, AT_EACCESS) == 0;
        child_exit(ok ? 0 : 1);
    }
    check(wait_child(pid) == 0, "access() checks the real user, AT_EACCESS the effective", 0);
    long rx = SC2(S_access, "/tmp/pt/pub", X_OK);               /* root, no x bit at all */
    check(rx == -EACCES, "even root can't run a file with no x bit", rx);

    const char *gone[] = { "/tmp/pt/closed/x", "/tmp/pt/secret", "/tmp/pt/pub", "/tmp/pt/open",
                           "/tmp/pt/nogroup", "/tmp/pt-sticky", "/tmp/pt-mine", 0 };
    for (int k = 0; gone[k]; k++)
        SC1(S_unlink, gone[k]);
    SC1(S_rmdir, "/tmp/pt/closed");
    SC1(S_rmdir, "/tmp/pt");
}

/* --- job control: stop, continue, and wait4 tells the parent ------------- */

#define SIGSTOP    19
#define SIGCONT    18
#define WUNTRACED  2
#define WCONTINUED 8
static void t_jobctl(void)
{
    long pid = SC0(S_fork);
    if (pid == 0)
        for (;;)
            SC0(S_pause);
    int st = 0, st2 = 0, st3 = 0;
    SC2(S_kill, pid, SIGSTOP);
    long w1 = SC4(S_wait4, pid, &st, WUNTRACED, 0);         /* "stopped by SIGSTOP" */
    SC2(S_kill, pid, SIGCONT);
    long w2 = SC4(S_wait4, pid, &st2, WCONTINUED, 0);       /* "continued": 0xFFFF */
    SC2(S_kill, pid, SIGSTOP);
    SC4(S_wait4, pid, &st, WUNTRACED, 0);
    SC2(S_kill, pid, SIGKILL);                              /* dies though stopped */
    long w3 = SC4(S_wait4, pid, &st3, 0, 0);
    int ok = w1 == pid && (st & 0xFF) == 0x7F && ((st >> 8) & 0xFF) == SIGSTOP &&
             w2 == pid && (st2 & 0xFFFF) == 0xFFFF && w3 == pid && (st3 & 0x7F) == SIGKILL;
    check(ok, "job control: SIGSTOP stops, wait4 says so; SIGCONT continues; SIGKILL ends it",
          !(w1 == pid) ? 1 : (st & 0xFF) != 0x7F ? 2 : w2 != pid ? 3 : (st2 & 0xFFFF) != 0xFFFF ? 4 : 5);
}

/* --- SA_RESTART: a read a signal cuts short runs again --------------------- */

#define SA_RESTART 0x10000000
/* Read one byte from an empty pipe; a SIGALRM comes at 20 ms, a child
   writes the byte at 80 ms. Returns what read() said. */
static long interrupted_read(unsigned long flags)
{
    int p[2] = { -1, -1 };
    SC1(S_pipe, p);
    sigaction_k(SIGALRM, (void *)on_alarm, flags);
    long pid = SC0(S_fork);
    if (pid == 0) {
        long ts[2] = { 0, 80000000 };
        SC2(S_nanosleep, ts, 0);
        SC3(S_write, p[1], "r", 1);
        child_exit(0);
    }
    got_sig = 0;
    long it[4] = { 0, 0, 0, 20000 };    /* once, in 20 ms */
    SC3(S_setitimer, 0, it, 0);
    char c = 0;
    long n = SC3(S_read, p[0], &c, 1);
    wait_child(pid);
    long rest = SC3(S_read, p[0], &c, 0);
    (void)rest;
    SC1(S_close, p[0]);
    SC1(S_close, p[1]);
    return n == 1 && c == 'r' && got_sig == SIGALRM ? 1 : n;
}

static void t_restart(void)
{
    long with = interrupted_read(SA_RESTART);
    long without = interrupted_read(0);
    check(with == 1 && without == -EINTR,
          "SA_RESTART: an interrupted pipe read runs again; without it, EINTR",
          with != 1 ? with : without);
}

/* --- umount: a disk file system comes off, and back --------------------- */

#define EBUSY 16
#define O_DIRECTORY 0200000
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static void t_umount(void)
{
    /* A disk mount to try it on: /data (ext2, i386's tests) or /mnt (FAT). */
    static char m[4096];
    char src[64] = "", dir[64] = "", type[16] = "";
    long fd = SC3(S_open, "/proc/mounts", O_RDONLY, 0);
    long n = fd >= 0 ? SC3(S_read, fd, m, sizeof m - 1) : 0;
    SC1(S_close, fd);
    m[n > 0 ? n : 0] = 0;
    for (char *l = m; *l && !dir[0]; ) {
        char *f[3];
        int k = 0;
        char *q = l;
        while (k < 3 && *q && *q != '\n') {
            f[k++] = q;
            while (*q && *q != ' ' && *q != '\n')
                q++;
            if (*q == ' ')
                *q++ = 0;
        }
        while (*q && *q != '\n')
            q++;
        char *next = *q ? q + 1 : q;
        *q = 0;
        if (k == 3 && starts(f[0], "/dev/") && (same(f[1], "/data", 6) || same(f[1], "/mnt", 5))) {
            for (int i = 0; f[0][i] && i < 63; i++) src[i] = f[0][i];
            for (int i = 0; f[1][i] && i < 63; i++) dir[i] = f[1][i];
            for (int i = 0; f[2][i] && i < 15; i++) type[i] = f[2][i];
        }
        l = next;
    }
    if (!dir[0]) {
        /* No /proc (i386's kernel tests run before /init mounts it): try
           /data as ext2, and find its disk by mounting it back below. */
        const char *dd = "/data", *et = "ext2";
        for (int i = 0; dd[i]; i++) dir[i] = dd[i];
        for (int i = 0; et[i]; i++) type[i] = et[i];
    }
    long d = SC3(S_open, dir, O_RDONLY | O_DIRECTORY, 0);
    long busy_open = SC2(S_umount2, dir, 0);
    if (busy_open == -EINVAL) {         /* not a mount after all: nothing to try */
        SC1(S_close, d);
        return;
    }
    SC1(S_close, d);
    SC1(S_chdir, dir);
    long busy_cwd = SC2(S_umount2, dir, 0);
    SC1(S_chdir, "/");
    long off = SC2(S_umount2, dir, 0);
    long again = SC2(S_umount2, dir, 0);    /* the directory under it: not a mount */
    long back = -1;
    if (src[0]) {
        back = SC5(S_mount, src, dir, type, 0, 0);
    } else {                            /* which disk was it? the one that mounts */
        static const char *const disks[] = { "/dev/hdc", "/dev/hdb", "/dev/hdd", "/dev/sdb",
                                             "/dev/sdc", "/dev/sda", 0 };
        for (int k = 0; disks[k] && back; k++)
            back = SC5(S_mount, disks[k], dir, type, 0, 0);
    }
    check(busy_open == -EBUSY && busy_cwd == -EBUSY && off == 0 && again == -EINVAL && back == 0,
          "umount: busy while open or a cwd, then off, then mounted again",
          busy_open != -EBUSY ? 1 : busy_cwd != -EBUSY ? 2 : off ? 3 : again != -EINVAL ? 4 : back);
}

/* --- PROT_NONE: no access at all, yet the page keeps its data -------------- */

#define SIGSEGV 11
static void t_protnone(void)
{
    char *m = (char *)SC6(S_mmap, 0, 4096, 3, 0x22, -1, 0);   /* RW, MAP_PRIVATE|MAP_ANON */
    m[0] = 'q';
    long r = SC3(S_mprotect, m, 4096, 0);
    long pid = SC0(S_fork);
    if (pid == 0) {
        volatile char c = m[0];         /* a read faults, not only a write */
        (void)c;
        child_exit(0);
    }
    int st = wait_child(pid);
    pid = SC0(S_fork);                  /* a child forked meanwhile has the data */
    if (pid == 0)
        child_exit(SC3(S_mprotect, m, 4096, 1) == 0 && m[0] == 'q' ? 0 : 1);
    int st2 = wait_child(pid);
    check(r == 0 && (st & 0x7F) == SIGSEGV && st2 == 0,
          "PROT_NONE: a read faults, the page keeps its data", st);
    SC2(S_munmap, m, 4096);
}

/* --- Unix sockets: SOCK_SEQPACKET, passing a file, the sender's credentials --- */

struct u_iov { const void *base; unsigned long len; };
struct u_msg {                          /* struct msghdr, natural layout on both CPUs */
    void *name; unsigned int namelen;
    struct u_iov *iov; unsigned long iovlen;
    void *control; unsigned long controllen;
    int flags;
};
struct u_cmsg { unsigned long len; int level, type; };     /* + data, word aligned */
#define CMSG_AL(n) (((n) + sizeof(long) - 1) & ~(sizeof(long) - 1))

static void t_unix_pass(void)
{
    int sv[2] = { -1, -1 }, p[2] = { -1, -1 }, one = 1, got = 0;
    unsigned int glen = 4;
    long r1 = SC4(S_socketpair, 1, 5, 0, sv);           /* AF_UNIX, SOCK_SEQPACKET */
    long r2 = SC5(S_setsockopt, sv[1], 1, 16, &one, 4); /* SO_PASSCRED */
    long r3 = SC5(S_getsockopt, sv[1], 1, 16, &got, &glen);
    SC1(S_pipe, p);
    SC3(S_write, p[1], "fd!", 3);

    /* One message carrying the pipe's read end; then our copy is closed. */
    long ctl[8] = { 0 };
    struct u_cmsg *cm = (struct u_cmsg *)ctl;
    cm->len = sizeof *cm + 4, cm->level = 1, cm->type = 1;   /* SOL_SOCKET, SCM_RIGHTS */
    *(int *)(cm + 1) = p[0];
    struct u_iov iov = { "hi", 2 };
    struct u_msg m = { 0, 0, &iov, 1, ctl, sizeof *cm + CMSG_AL(4), 0 };
    long r4 = SC3(S_sendmsg, sv[0], &m, 0);
    SC1(S_close, p[0]);

    char buf[8];
    long ctl2[16] = { 0 };
    struct u_iov iov2 = { buf, sizeof buf };
    struct u_msg m2 = { 0, 0, &iov2, 1, ctl2, sizeof ctl2, 0 };
    long r5 = SC3(S_recvmsg, sv[1], &m2, 0);

    /* First the credentials (SCM_CREDENTIALS: pid, uid, gid), then the file. */
    struct u_cmsg *c1 = (struct u_cmsg *)ctl2;
    struct u_cmsg *c2 = (struct u_cmsg *)((char *)ctl2 + CMSG_AL(c1->len));
    int pid_ok = c1->type == 2 && ((int *)(c1 + 1))[0] == SC0(S_getpid);
    int fd = c2->type == 1 ? *(int *)(c2 + 1) : -1;
    char got3[4] = { 0 };
    long n = fd >= 0 ? SC3(S_read, fd, got3, 3) : -1;
    check(r1 == 0 && r2 == 0 && r3 == 0 && got == 1 && r4 == 2 && r5 == 2 && pid_ok &&
          n == 3 && same(got3, "fd!", 3), "SOCK_SEQPACKET + SCM_RIGHTS + SCM_CREDENTIALS",
          r1 ? r1 : r3 ? r3 : r4 != 2 ? r4 : r5 != 2 ? r5 : n);
    if (fd >= 0)
        SC1(S_close, fd);
    SC1(S_close, p[1]);
    SC1(S_close, sv[0]);
    SC1(S_close, sv[1]);
}

/* 3 MB through a stream socketpair in odd-sized pieces, a child checking
   every byte: the ring grows from one page while both ends are busy, and
   wraps at every size (net/unix.c, ring_grow). */
static unsigned char big_out[100003], big_in[65536];
static unsigned char big_byte(unsigned long i) { return (unsigned char)(i * 7 + i / 4096); }

static void t_unix_big(void)
{
    int sv[2] = { -1, -1 };
    long r = SC4(S_socketpair, 1, 1, 0, sv);            /* AF_UNIX, SOCK_STREAM */
    if (r) {
        check(0, "unix stream: 3 MB, every byte", r);
        return;
    }
    const unsigned long total = 3u << 20;
    long pid = SC0(S_fork);
    if (pid == 0) {                                     /* the reader */
        SC1(S_close, sv[0]);
        unsigned long got = 0;
        int bad = 0;
        for (;;) {
            long n = SC3(S_read, sv[1], big_in, sizeof big_in - 1000);
            if (n <= 0)
                break;
            for (long k = 0; k < n; k++)
                bad |= big_in[k] != big_byte(got + (unsigned long)k);
            got += (unsigned long)n;
        }
        SC1(S_exit, bad ? 2 : got == total ? 0 : 3);
    }
    SC1(S_close, sv[1]);
    unsigned long sent = 0;
    long w = 0, t0 = now_ms();
    while (sent < total) {
        unsigned long k = total - sent < sizeof big_out ? total - sent : sizeof big_out;
        for (unsigned long j = 0; j < k; j++)
            big_out[j] = big_byte(sent + j);
        w = SC3(S_write, sv[0], big_out, k);
        if (w <= 0)
            break;
        sent += (unsigned long)w;                       /* a short write: the rest next time */
    }
    SC1(S_close, sv[0]);
    int status = -1;
    SC4(S_wait4, pid, &status, 0, 0);
    check(sent == total && status == 0, "unix stream: 3 MB, every byte",
          sent != total ? w : status);
    out("  (3 MB in "), outn(now_ms() - t0), out(" ms)\n");
}

/* ppoll's timeout, checked as Linux checks it: a negative time, or
   nanoseconds past 999999999, is EINVAL (i386: the 32-bit timespec; it
   used to overflow past ~24 days into "no limit"). A huge one is fine. */
static void t_ppoll_time(void)
{
    int p[2] = { -1, -1 };
    SC1(S_pipe, p);
    SC3(S_write, p[1], "x", 1);         /* ready: a valid wait returns at once */
    struct { int fd; short events, revents; } pf = { p[0], 1, 0 };
    long neg[2] = { -1, 0 }, nsec[2] = { 0, 1000000000 }, huge[2] = { 0x7FFFFFFF, 999999999 };
    long a = SC5(S_ppoll, &pf, 1, neg, 0, 8);
    long b = SC5(S_ppoll, &pf, 1, nsec, 0, 8);
    long c = SC5(S_ppoll, &pf, 1, huge, 0, 8);
    check(a == -EINVAL && b == -EINVAL && c == 1, "ppoll: bad timeouts are EINVAL, a huge one waits",
          a != -EINVAL ? a : b != -EINVAL ? b : c);
    SC1(S_close, p[0]);
    SC1(S_close, p[1]);
}

/* poll() on an epoll file: ready only while something inside is (GLib
   watches libmount's monitor so; "always ready" spun the Save As dialog). */
static void t_epoll_poll(void)
{
    int p[2] = { -1, -1 };
    SC1(S_pipe, p);
    long ep = SC1(S_epoll_create1, 0);
    unsigned int ev[3] = { 1, 7, 0 };                   /* EPOLLIN, data 7 */
    long add = SC4(S_epoll_ctl, ep, 1, p[0], ev);       /* EPOLL_CTL_ADD */
    struct { int fd; short events, revents; } pf = { (int)ep, 1, 0 };
    long before = SC3(S_poll, &pf, 1, 0);               /* empty pipe: nothing */
    SC3(S_write, p[1], "x", 1);
    pf.revents = 0;
    long after = SC3(S_poll, &pf, 1, 0);
    check(ep >= 0 && add == 0 && before == 0 && after == 1 && (pf.revents & 1),
          "poll on an epoll file: ready only when an entry is", before ? before : after);
    SC1(S_close, ep);
    SC1(S_close, p[0]);
    SC1(S_close, p[1]);
}

void cmain(void)
{
    t_cputime();
    t_clocks();
    t_sendfile();
    t_copy_range();
    t_signals();
    t_sem();
    t_msg();
    t_shm();
    t_dirflag();
    t_inotify();
    t_locks();
    t_misc();
    t_perm();
    t_protnone();
    t_fileperm();
    t_umount();
    t_restart();
    t_jobctl();
    t_netlink();
    t_packet();
    t_nl80211();
    t_tcp_listen();
    t_unix_pass();
    t_unix_big();
    t_epoll_poll();
    t_ppoll_time();
    /* Leave /tmp as we found it: its files are memory (`make test` counts
       every page after the programs have run). */
    SC1(S_unlink, "/tmp/sf_src");
    SC1(S_unlink, "/tmp/sf_dst");
    SC1(S_unlink, "/tmp/cfr_dst");
    SC1(S_unlink, "/tmp/lockf");
    SC1(S_rmdir, "/tmp/itest");
    SC1(S_rmdir, "/tmp/jail");
    if (failed) {
        out("systest: ");
        outn(failed);
        out(" failed\n");
    } else {
        out("systest: all ok\n");
    }
    SC1(S_exit_group, failed != 0);
}
