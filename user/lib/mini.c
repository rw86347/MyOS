/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * mini.c — the tiny C library for MyOS test programs (see mini.h)
 */
#include <mini.h>

#define AT_NULL     0
#define AT_SYSINFO 32           /* address of the kernel's syscall entry */

uintptr_t __vsyscall;

int main(int argc, char **argv);

/* Called by crt0.asm with a pointer to argc on the start-up stack:
   argc, argv[0..argc-1], NULL, envp..., NULL, auxv pairs..., AT_NULL. */
void __libc_start(uint32_t *sp) __attribute__((noreturn));
void __libc_start(uint32_t *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)&sp[1];
    char **envp = argv + argc + 1;
    while (*envp)
        envp++;
    for (uint32_t *aux = (uint32_t *)(envp + 1); aux[0] != AT_NULL; aux += 2)
        if (aux[0] == AT_SYSINFO)
            __vsyscall = aux[1];

    _exit(main(argc, argv));
}

long write(int fd, const void *buf, size_t n)
{
    return syscall3(SYS_write, fd, (long)buf, (long)n);
}

long read(int fd, void *buf, size_t n)
{
    return syscall3(SYS_read, fd, (long)buf, (long)n);
}

int open(const char *path, int flags)
{
    return (int)syscall3(SYS_open, (long)path, flags, 0);
}

int close(int fd)
{
    return (int)syscall3(SYS_close, fd, 0, 0);
}

long lseek(int fd, long off, int whence)
{
    return syscall3(SYS_lseek, fd, off, whence);
}

int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *st)
{
    return (int)syscall5(SYS_statx, dirfd, (long)path, flags, (long)mask, (long)st);
}

int fork(void)
{
    return (int)syscall3(SYS_fork, 0, 0, 0);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)syscall3(SYS_execve, (long)path, (long)argv, (long)envp);
}

int wait4(int pid, int *status, int options, void *rusage)
{
    return (int)syscall5(SYS_wait4, pid, (long)status, options, (long)rusage, 0);
}

int getpid(void)  { return (int)syscall3(SYS_getpid, 0, 0, 0); }
int getppid(void) { return (int)syscall3(SYS_getppid, 0, 0, 0); }

void *sys_brk(void *addr)
{
    return (void *)syscall3(SYS_brk, (long)addr, 0, 0);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    return (void *)syscall6_int80(SYS_mmap2, (long)addr, (long)len, prot, flags, fd,
                                  off / 4096);
}

int munmap(void *addr, size_t len)
{
    return (int)syscall3(SYS_munmap, (long)addr, (long)len, 0);
}

int pipe(int fds[2])            { return (int)syscall3(SYS_pipe, (long)fds, 0, 0); }
int dup(int fd)                 { return (int)syscall3(SYS_dup, fd, 0, 0); }
int dup2(int old, int new_fd)   { return (int)syscall3(SYS_dup2, old, new_fd, 0); }
int fcntl(int fd, int cmd, long arg) { return (int)syscall3(SYS_fcntl64, fd, cmd, arg); }
int getdents64(int fd, void *buf, size_t n) { return (int)syscall3(SYS_getdents64, fd, (long)buf, (long)n); }
int chdir(const char *path)     { return (int)syscall3(SYS_chdir, (long)path, 0, 0); }
long getcwd(char *buf, size_t n) { return syscall3(SYS_getcwd, (long)buf, (long)n, 0); }
long writev(int fd, const struct iovec *iov, int n) { return syscall3(SYS_writev, fd, (long)iov, n); }
long readlink(const char *p, char *buf, size_t n) { return syscall3(SYS_readlink, (long)p, (long)buf, (long)n); }

int kill(int pid, int sig)      { return (int)syscall3(SYS_kill, pid, sig, 0); }
int ioctl(int fd, unsigned long req, void *arg) { return (int)syscall3(SYS_ioctl, fd, (long)req, (long)arg); }
int setpgid(int pid, int pgid)  { return (int)syscall3(SYS_setpgid, pid, pgid, 0); }
int getpgrp(void)               { return (int)syscall3(SYS_getpgrp, 0, 0, 0); }
int sched_yield(void)           { return (int)syscall3(SYS_sched_yield, 0, 0, 0); }

/* The kernel's struct sigaction for rt_sigaction (i386). */
struct k_sigaction { void *handler; unsigned long flags; void (*restorer)(void); sigset_t mask; };
void __restore(void), __restore_rt(void);   /* crt0.asm: call sigreturn */

int sigaction_simple(int sig, void (*handler)(int), unsigned flags, sigset_t mask)
{
    struct k_sigaction a = { (void *)handler, flags | SA_RESTORER,
                             (flags & SA_SIGINFO) ? __restore_rt : __restore, mask };
    return (int)syscall5(SYS_rt_sigaction, sig, (long)&a, 0, 8, 0);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return (int)syscall5(SYS_rt_sigprocmask, how, (long)set, (long)old, 8, 0);
}

int sigpending(sigset_t *set)    { return (int)syscall3(SYS_rt_sigpending, (long)set, 8, 0); }
int sigsuspend(const sigset_t *set) { return (int)syscall3(SYS_rt_sigsuspend, (long)set, 8, 0); }

void _exit(int code)
{
    for (;;)
        syscall3(SYS_exit, code, 0, 0);
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = d;
    const unsigned char *ss = s;
    while (n--)
        *dd++ = *ss++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dd = d;
    while (n--)
        *dd++ = (unsigned char)c;
    return d;
}

void put(const char *s)
{
    write(1, s, strlen(s));
}

void put_u64(uint64_t v)
{
    char buf[21];
    int i = sizeof buf;
    buf[--i] = '\0';
    do {
        buf[--i] = '0' + (char)(v % 10);
        v /= 10;
    } while (v);
    put(&buf[i]);
}
