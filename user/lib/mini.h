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
#define SYS_read     3
#define SYS_write    4
#define SYS_open     5
#define SYS_close    6
#define SYS_lseek   19
#define SYS_getpid  20
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
#define EBADF   9
#define EFAULT 14
#define ENOTDIR 20
#define ESPIPE 29
#define EROFS  30
#define STATX_BASIC_STATS 0x7FF

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

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    memcmp(const void *a, const void *b, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);

/* Print to stdout: a string, an unsigned number. */
void put(const char *s);
void put_u64(uint64_t v);

#endif
