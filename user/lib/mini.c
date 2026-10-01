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
