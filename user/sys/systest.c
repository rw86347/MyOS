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
       S_getsockname = 51 };
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
       S_sendto = 369, S_recvfrom = 371, S_setsockopt = 366 };
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
#define EAGAIN 11
#define EINTR 4
#define EINVAL 22
#define ENOMSG 42

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
    t_inotify();
    t_locks();
    t_misc();
    t_netlink();
    t_packet();
    t_nl80211();
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
