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
       S_inotify_init1 = 294, S_syncfs = 306 };
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
       S_inotify_init1 = 332, S_syncfs = 344 };
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
    check(size == 16384 && n > 0 && contains(buf, n, "MyOS"), "syslog (dmesg)", n);

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

void cmain(void)
{
    t_cputime();
    t_clocks();
    t_sendfile();
    t_signals();
    t_sem();
    t_msg();
    t_shm();
    t_inotify();
    t_locks();
    t_misc();
    if (failed) {
        out("systest: ");
        outn(failed);
        out(" failed\n");
    } else {
        out("systest: all ok\n");
    }
    SC1(S_exit_group, failed != 0);
}
