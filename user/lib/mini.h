/*
 * mini.h — a tiny C library for MyOS test programs
 *
 * NOT musl and not a real libc: just enough to write tests and benchmarks
 * in C. Everything talks to the kernel with real Linux i386 system calls,
 * so these programs would run on Linux too.
 *
 * Two ways into the kernel:
 *   int 0x80   always works
 *   vsyscall   the entry the kernel offers in the auxiliary vector
 *              (AT_SYSINFO) — uses `sysenter`, much faster on old CPUs.
 *              musl and glibc use it automatically; so do we.
 */
#ifndef MINI_H
#define MINI_H

#include <stddef.h>
#include <stdint.h>

/* Linux i386 syscall numbers */
#define SYS_exit     1
#define SYS_fork     2
#define SYS_read     3
#define SYS_write    4
#define SYS_open     5
#define SYS_close    6
#define SYS_execve  11
#define SYS_chdir   12
#define SYS_lseek   19
#define SYS_getpid  20
#define SYS_dup     41
#define SYS_pipe    42
#define SYS_brk     45
#define SYS_dup2    63
#define SYS_getppid 64
#define SYS_readlink 85
#define SYS_munmap  91
#define SYS_wait4  114
#define SYS_kill    37
#define SYS_ioctl   54
#define SYS_setpgid 57
#define SYS_getpgrp 65
#define SYS_writev 146
#define SYS_sched_yield 158
#define SYS_rt_sigaction 174
#define SYS_rt_sigprocmask 175
#define SYS_rt_sigpending 176
#define SYS_rt_sigsuspend 179
#define SYS_getcwd 183
#define SYS_mmap2  192
#define SYS_getdents64 220
#define SYS_fcntl64 221
#define SYS_statx  383

/* Linux values */
#define O_RDONLY    0
#define O_WRONLY    1
#define AT_FDCWD    (-100)
#define AT_EMPTY_PATH 0x1000
#define SEEK_SET    0
#define S_IFMT      0170000
#define S_IFREG     0100000
#define S_IFDIR     0040000
#define ENOENT  2
#define ECHILD 10
#define EBADF   9
#define EFAULT 14
#define ENOTDIR 20
#define EAGAIN 11
#define ESPIPE 29
#define EPIPE  32
#define EROFS  30
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_DIRECTORY 0200000
#define S_IFLNK     0120000
#define S_IFCHR     0020000
#define EEXIST 17
#define EISDIR 21
#define EINVAL 22
#define EACCES 13
#define ENOTEMPTY 39
#define STATX_BASIC_STATS 0x7FF
#define WNOHANG 1
#define O_NONBLOCK 04000
#define O_CLOEXEC  02000000
#define F_GETFD 1
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
struct iovec { void *base; size_t len; };

/* Signals (Linux numbers) */
#define SIGINT   2
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SA_SIGINFO  4
#define SA_RESTORER 0x04000000
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define EINTR 4
#define ENOTTY 25
typedef unsigned long long sigset_t;
#define SIGBIT(s) (1ull << ((s) - 1))
struct siginfo { int si_signo, si_errno, si_code; int pad[29]; };

/* Terminal (Linux's struct termios and ioctls) */
struct termios {
    unsigned c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line, c_cc[19];
};
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define ICANON 0000002
#define ECHO   0000010
#define VMIN   6
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s) (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)    ((s) & 0x7F)

/* Linux's struct statx (256 bytes) — what stat() returns. */
struct statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t reserved; };
struct statx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode, spare0;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align, stx_dio_offset_align;
    uint64_t spare3[12];
};

/* The kernel's sysenter entry, from AT_SYSINFO (0 = not offered). */
extern uintptr_t __vsyscall;

/* Syscall via int 0x80. EAX = number, EBX/ECX/EDX = arguments. */
static inline long sys_int80(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/* Syscall via the vsyscall entry (sysenter). Same registers. */
static inline long sys_vsys(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile ("call *%5" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c),
                      "m"(__vsyscall) : "memory");
    return r;
}

/* The normal way: vsyscall if the kernel offers it. */
static inline long syscall3(long n, long a, long b, long c)
{
    return __vsyscall ? sys_vsys(n, a, b, c) : sys_int80(n, a, b, c);
}

/* Six arguments: the 6th goes in EBP. int 0x80 only (the vsyscall entry
   uses EBP itself) — fine for the rare mmap. */
static inline long syscall6_int80(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    __asm__ volatile ("push %%ebp\n\t"
                      "mov %7, %%ebp\n\t"
                      "int $0x80\n\t"
                      "pop %%ebp"
                      : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "m"(f)
                      : "memory");
    return r;
}

/* Five arguments: ESI and EDI carry the 4th and 5th. */
static inline long syscall5(long n, long a, long b, long c, long d, long e)
{
    long r;
    if (__vsyscall)
        __asm__ volatile ("call *%7" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c),
                          "S"(d), "D"(e), "m"(__vsyscall) : "memory");
    else
        __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c),
                          "S"(d), "D"(e) : "memory");
    return r;
}

/* Read the CPU's cycle counter (allowed in user mode). */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
int  open(const char *path, int flags);
int  close(int fd);
long lseek(int fd, long off, int whence);
int  statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *st);
void _exit(int code) __attribute__((noreturn));
int  fork(void);
int  execve(const char *path, char *const argv[], char *const envp[]);
int  wait4(int pid, int *status, int options, void *rusage);
int  getpid(void);
int  getppid(void);
void *sys_brk(void *addr);           /* the raw syscall: returns the break */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int  munmap(void *addr, size_t len);
int  pipe(int fds[2]);
int  dup(int fd);
int  dup2(int old, int new_fd);
int  fcntl(int fd, int cmd, long arg);
int  getdents64(int fd, void *buf, size_t n);
int  chdir(const char *path);
long getcwd(char *buf, size_t size);
long writev(int fd, const struct iovec *iov, int n);
long readlink(const char *path, char *buf, size_t n);
int  kill(int pid, int sig);
int  ioctl(int fd, unsigned long req, void *arg);
int  setpgid(int pid, int pgid);
int  getpgrp(void);
int  sched_yield(void);
/* Install a handler (SA_RESTORER and the restorer are added, like musl). */
int  sigaction_simple(int sig, void (*handler)(int), unsigned flags, sigset_t mask);
int  sigprocmask(int how, const sigset_t *set, sigset_t *old);
int  sigpending(sigset_t *set);
int  sigsuspend(const sigset_t *set);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    memcmp(const void *a, const void *b, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);

/* Print to stdout: a string, an unsigned number. */
void put(const char *s);
void put_u64(uint64_t v);

#endif
