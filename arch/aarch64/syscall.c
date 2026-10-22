/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * syscall.c — EL0 SVC dispatch (step 14b: first BusyBox wave)
 */
#include "syscall.h"

#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>

#include "arm64_timer.h"
#include "cpio.h"
#include "elf64_load.h"
#include "proc.h"
#include "uaccess.h"
#include "uart.h"

#define EBADF   9
#define EFAULT  14
#define EINVAL  22
#define ENOTTY  25
#define ENOSYS  38
#define ENOENT  2
#define ENOMEM  12

#define MAP_ANONYMOUS 0x20
#define TIOCGWINSZ    0x5413u
#define S_IFCHR       0020000
#define S_IFREG       0100000
#define S_IFDIR       0040000
#define STAT_SIZE     128
#define AT_FDCWD      (-100)
#define AT_EMPTY_PATH 0x1000
#define TCGETS        0x5401u
#define TCSETS        0x5402u
#define ICANON        0x0002u
#define ECHO          0x0008u
#define ISIG          0x0001u

struct cons_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};

static struct cons_termios cons_tty = {
    .c_iflag = 0x0001u, /* ICRNL */
    .c_oflag = 0x0005u, /* OPOST|ONLCR */
    .c_cflag = 0x000001b8u,
    .c_lflag = ISIG | ICANON | ECHO,
};
#define TIOCGPGRP     0x540Fu
#define TIOCSPGRP     0x5410u
#define MAX_FD        ARM64_MAX_FD

static struct arm64_fd *fds(void)
{
    return arm64_cur_fds();
}

static void fill_stat(uint8_t *st, uint32_t mode, uint64_t size, uint64_t ino)
{
    uint64_t dev = 1;

    memset(st, 0, STAT_SIZE);
    memcpy(st, &dev, 8);
    memcpy(st + 8, &ino, 8);
    st[16] = (uint8_t)(mode & 0xff);
    st[17] = (uint8_t)((mode >> 8) & 0xff);
    st[20] = 1; /* st_nlink */
    memcpy(st + 48, &size, 8);
    st[56] = 0x00;
    st[57] = 0x10; /* st_blksize = 4096 */
}

static volatile int s_el0_pass;
static void *s_el1_resume_pc;
static void *s_user_entry;
static void *s_user_sp;
static uint64_t s_user_brk = ARM64_USER_IMAGE_VA + ARM64_USER_IMAGE_SIZE;
static uint64_t s_mmap_bump = ARM64_MMAP_BASE;

void el0_set_resume(void *el1_pc)
{
    s_el1_resume_pc = el1_pc;
}

void el0_set_user_brk(uint64_t brk)
{
    s_user_brk = brk;
    if (arm64_current())
        arm64_current()->brk = brk;
}

extern void arm64_return_to_el1(void *el1_pc);

int el0_test_passed(void)
{
    return s_el0_pass;
}

static uint64_t sysret_err(int err)
{
    return (uint64_t)(-(long)err);
}

static uint64_t sys_write(struct el0_regs *regs)
{
    int fd = (int)regs->x[0];
    const void *buf = (const void *)(uintptr_t)regs->x[1];
    size_t count = (size_t)regs->x[2];
    char tmp[128];
    size_t chunk;
    size_t total = 0;

    if (fd >= 0 && fd < MAX_FD && fds()[fd].kind == FD_PIPE_W) {
        struct arm64_pipe *p = fds()[fd].pipe;
        char tmp[128];
        size_t chunk, room;

        if (!p)
            return sysret_err(EBADF);
        if (!p->rrefs)
            return sysret_err(32); /* EPIPE */
        room = ARM64_PIPE_CAP - (p->r + p->n);
        if (!room) {
            arm64_block_retry();
            return 0;
        }
        chunk = count < room ? count : room;
        if (chunk > sizeof tmp)
            chunk = sizeof tmp;
        if (copy_from_user(tmp, buf, chunk) < 0)
            return sysret_err(EFAULT);
        memcpy(p->buf + p->r + p->n, tmp, chunk);
        p->n += chunk;
        arm64_pipe_wake(p);
        return chunk;
    }
    if (fd != 1 && fd != 2)
        return sysret_err(EBADF);
    while (total < count) {
        chunk = count - total;
        if (chunk > sizeof tmp)
            chunk = sizeof tmp;
        if (copy_from_user(tmp, (const char *)buf + total, chunk) < 0)
            return sysret_err(EFAULT);
        for (size_t i = 0; i < chunk; i++) {
            char c = tmp[i];

            if (c == '\n')
                uart_putc('\r');
            uart_putc(c);
        }
        total += chunk;
    }
    s_el0_pass = 1;
    return total;
}

static uint64_t sys_writev(struct el0_regs *regs)
{
    struct iovec {
        uint64_t base;
        uint64_t len;
    } iov[8];
    int fd = (int)regs->x[0];
    uint64_t iovptr = regs->x[1];
    int iovcnt = (int)regs->x[2];
    uint64_t total = 0;
    int i;

    if (fd != 1 && fd != 2 &&
        !(fd >= 0 && fd < MAX_FD && fds()[fd].kind == FD_PIPE_W))
        return sysret_err(EBADF);
    if (iovcnt <= 0 || iovcnt > 8)
        return sysret_err(EINVAL);
    if (copy_from_user(iov, (const void *)(uintptr_t)iovptr,
                       (size_t)iovcnt * sizeof iov[0]) < 0)
        return sysret_err(EFAULT);
    for (i = 0; i < iovcnt; i++) {
        struct el0_regs one = *regs;
        uint64_t n;

        one.x[0] = (uint64_t)fd;
        one.x[1] = iov[i].base;
        one.x[2] = iov[i].len;
        n = sys_write(&one);
        if ((int64_t)n < 0)
            return n;
        total += n;
    }
    return total;
}

static uint64_t sys_fstat_fd(int fd, uint64_t dst)
{
    uint8_t st[STAT_SIZE];

    if (fd < 0 || fd >= MAX_FD || fds()[fd].kind == FD_NONE)
        return sysret_err(EBADF);
    if (fds()[fd].kind == FD_CONS)
        fill_stat(st, S_IFCHR | 0666, 0, 1);
    else if (fds()[fd].kind == FD_DIR)
        fill_stat(st, S_IFDIR | 0755, 0, 2);
    else
        fill_stat(st, S_IFREG | 0644, fds()[fd].len,
                  (uint64_t)(uintptr_t)fds()[fd].data);
    if (copy_to_user((void *)(uintptr_t)dst, st, sizeof st) < 0)
        return sysret_err(EFAULT);
    return 0;
}

static int path_from_user(uint64_t uptr, char *out, size_t outsz)
{
    size_t i;

    if (!outsz)
        return -1;
    memset(out, 0, outsz);
    for (i = 0; i < outsz - 1; i++) {
        if (copy_from_user(out + i, (const char *)(uintptr_t)uptr + i, 1) < 0)
            return -1;
        if (out[i] == '\0')
            return out[0] ? 0 : -1;
    }
    return -1;
}

static int alloc_file_fd(const void *data, size_t len)
{
    int fd;

    for (fd = 3; fd < MAX_FD; fd++) {
        if (fds()[fd].kind == FD_NONE) {
            fds()[fd].kind = FD_FILE;
            fds()[fd].data = data;
            fds()[fd].len = len;
            fds()[fd].pos = 0;
            fds()[fd].pipe = 0;
            return fd;
        }
    }
    return -1;
}

static uint64_t sys_openat(struct el0_regs *regs)
{
    char rel[256], path[256];
    const void *data;
    size_t len;
    uint32_t mode;

    if (path_from_user(regs->x[1], rel, sizeof rel) < 0)
        return sysret_err(EFAULT);
    arm64_abs_path(path, sizeof path, rel);
    if (arm64_resolve_path(path, &mode, &data, &len) < 0)
        return sysret_err(ENOENT);
    if ((mode & ARM64_S_IFMT) == ARM64_S_IFDIR) {
        int fd = alloc_file_fd(0, 0);

        if (fd < 0)
            return sysret_err(24);
        fds()[fd].kind = FD_DIR;
        memset(fds()[fd].prefix, 0, sizeof fds()[fd].prefix);
        memcpy(fds()[fd].prefix, path, strlen(path) < 63 ? strlen(path) : 63);
        return (uint64_t)fd;
    }
    {
        int fd = alloc_file_fd(data, len);

        if (fd < 0)
            return sysret_err(24);
        return (uint64_t)fd;
    }
}

static uint64_t sys_read(struct el0_regs *regs)
{
    int fd = (int)regs->x[0];
    size_t count = (size_t)regs->x[2];
    size_t n;

    if (fd < 0 || fd >= MAX_FD || fds()[fd].kind == FD_NONE)
        return sysret_err(EBADF);
    if (fds()[fd].kind == FD_CONS) {
        static char line[128];
        static size_t ln;
        size_t got = 0;
        int canon = (cons_tty.c_lflag & ICANON) != 0;

        while (got < count) {
            int c = uart_getc();

            if (c < 0) {
                if (got)
                    break;
                __asm__ volatile("wfe");
                continue;
            }
            if (c == '\r')
                c = '\n';
            if (canon) {
                if (cons_tty.c_lflag & ECHO)
                    uart_putc((char)c);
                if (c == '\n' || ln + 1 >= sizeof line) {
                    line[ln++] = (char)c;
                    if (copy_to_user((void *)(uintptr_t)regs->x[1], line, ln) < 0)
                        return sysret_err(EFAULT);
                    got = ln;
                    ln = 0;
                    return got;
                }
                line[ln++] = (char)c;
            } else {
                char ch = (char)c;

                if (cons_tty.c_lflag & ECHO)
                    uart_putc(ch);
                if (copy_to_user((void *)(uintptr_t)(regs->x[1] + got), &ch, 1) < 0)
                    return sysret_err(EFAULT);
                got++;
                return got;
            }
        }
        return got;
    }
    if (fds()[fd].kind == FD_PIPE_R) {
        struct arm64_pipe *p = fds()[fd].pipe;
        size_t n;

        if (!p)
            return sysret_err(EBADF);
        if (!p->n) {
            if (!p->wrefs)
                return 0;
            arm64_block_retry();
            return 0;
        }
        n = p->n < count ? p->n : count;
        if (copy_to_user((void *)(uintptr_t)regs->x[1], p->buf + p->r, n) < 0)
            return sysret_err(EFAULT);
        p->r += n;
        p->n -= n;
        if (!p->n)
            p->r = 0;
        arm64_pipe_wake(p);
        return n;
    }
    if (fds()[fd].kind != FD_FILE || fds()[fd].pos >= fds()[fd].len)
        return 0;
    n = fds()[fd].len - fds()[fd].pos;
    if (n > count)
        n = count;
    if (copy_to_user((void *)(uintptr_t)regs->x[1], fds()[fd].data + fds()[fd].pos, n) < 0)
        return sysret_err(EFAULT);
    fds()[fd].pos += n;
    return n;
}

static uint64_t sys_pread64(struct el0_regs *regs)
{
    int fd = (int)regs->x[0];
    size_t count = (size_t)regs->x[2];
    uint64_t off = regs->x[3];
    size_t n;

    if (fd < 0 || fd >= MAX_FD || fds()[fd].kind != FD_FILE)
        return sysret_err(EBADF);
    if (off >= fds()[fd].len)
        return 0;
    n = fds()[fd].len - (size_t)off;
    if (n > count)
        n = count;
    if (copy_to_user((void *)(uintptr_t)regs->x[1], fds()[fd].data + (size_t)off, n) < 0)
        return sysret_err(EFAULT);
    return n;
}

static uint64_t sys_close(struct el0_regs *regs)
{
    int fd = (int)regs->x[0];

    if (fd < 0 || fd >= MAX_FD || fds()[fd].kind == FD_NONE)
        return sysret_err(EBADF);
    if (fd >= 3) {
        if (fds()[fd].pipe) {
            if (fds()[fd].kind == FD_PIPE_R && fds()[fd].pipe->rrefs)
                fds()[fd].pipe->rrefs--;
            if (fds()[fd].kind == FD_PIPE_W && fds()[fd].pipe->wrefs)
                fds()[fd].pipe->wrefs--;
            arm64_pipe_wake(fds()[fd].pipe);
        }
        fds()[fd].kind = FD_NONE;
        fds()[fd].pipe = 0;
    }
    return 0;
}

static uint64_t sys_lseek(struct el0_regs *regs)
{
    int fd = (int)regs->x[0];
    int64_t off = (int64_t)regs->x[1];
    int whence = (int)regs->x[2];
    int64_t pos;

    if (fd < 0 || fd >= MAX_FD || fds()[fd].kind != FD_FILE)
        return sysret_err(EBADF);
    if (whence == 0)
        pos = off;
    else if (whence == 1)
        pos = (int64_t)fds()[fd].pos + off;
    else if (whence == 2)
        pos = (int64_t)fds()[fd].len + off;
    else
        return sysret_err(EINVAL);
    if (pos < 0)
        return sysret_err(EINVAL);
    fds()[fd].pos = (size_t)pos;
    return (uint64_t)pos;
}

static uint64_t sys_newfstatat(struct el0_regs *regs)
{
    int dfd = (int)regs->x[0];
    uint64_t upath = regs->x[1];
    char path[256];
    uint8_t st[STAT_SIZE];
    const void *data;
    size_t len;

    if (!upath || ((int)regs->x[3] & AT_EMPTY_PATH)) {
        if (dfd == AT_FDCWD)
            return sysret_err(ENOENT);
        return sys_fstat_fd(dfd, regs->x[2]);
    }
    if (path_from_user(upath, path, sizeof path) < 0)
        return sysret_err(EFAULT);
    {
        char abs[256];
        uint32_t mode = 0;

        arm64_abs_path(abs, sizeof abs, path);
        if (arm64_resolve_path(abs, &mode, &data, &len) < 0)
            return sysret_err(ENOENT);
        if ((mode & ARM64_S_IFMT) == ARM64_S_IFDIR)
            fill_stat(st, S_IFDIR | 0755, 0, 2);
        else
            fill_stat(st, S_IFREG | 0644, len, (uint64_t)(uintptr_t)data);
    }
    if (copy_to_user((void *)(uintptr_t)regs->x[2], st, sizeof st) < 0)
        return sysret_err(EFAULT);
    return 0;
}

static uint64_t sys_faccessat(struct el0_regs *regs)
{
    char path[256];
    const void *data;
    size_t len;

    if (path_from_user(regs->x[1], path, sizeof path) < 0)
        return sysret_err(EFAULT);
    {
        char abs[256];
        uint32_t mode = 0;

        arm64_abs_path(abs, sizeof abs, path);
        if (arm64_resolve_path(abs, &mode, &data, &len) < 0)
            return sysret_err(ENOENT);
    }
    return 0;
}

static uint64_t sys_getdents64(struct el0_regs *regs)
{
    int fd = (int)regs->x[0];
    char tmp[512];
    int n;

    if (fd < 0 || fd >= MAX_FD || fds()[fd].kind != FD_DIR)
        return sysret_err(EBADF);
    n = arm64_cpio_getdents(arm64_initrd_start,
                            (size_t)(arm64_initrd_end - arm64_initrd_start),
                            fds()[fd].prefix, tmp, sizeof tmp);
    if (n < 0)
        return sysret_err(EINVAL);
    if (fds()[fd].pos) {
        fds()[fd].pos = 0;
        return 0;
    }
    if (copy_to_user((void *)(uintptr_t)regs->x[1], tmp, (size_t)n) < 0)
        return sysret_err(EFAULT);
    fds()[fd].pos = 1;
    return (uint64_t)n;
}

static uint64_t sys_chdir(struct el0_regs *regs)
{
    char rel[256], path[256];
    const void *data;
    size_t len;
    uint32_t mode;

    if (!arm64_current() || path_from_user(regs->x[0], rel, sizeof rel) < 0)
        return sysret_err(EFAULT);
    arm64_abs_path(path, sizeof path, rel);
    if (arm64_resolve_path(path, &mode, &data, &len) < 0)
        return sysret_err(ENOENT);
    if ((mode & ARM64_S_IFMT) != ARM64_S_IFDIR)
        return sysret_err(20); /* ENOTDIR */
    memset(arm64_current()->cwd, 0, sizeof arm64_current()->cwd);
    memcpy(arm64_current()->cwd, path,
           strlen(path) < 63 ? strlen(path) : 63);
    return 0;
}

static uint64_t sys_readlinkat(struct el0_regs *regs)
{
    char rel[256], path[256];
    const void *data;
    size_t len;
    uint32_t mode;
    size_t n;

    if (path_from_user(regs->x[1], rel, sizeof rel) < 0)
        return sysret_err(EFAULT);
    arm64_abs_path(path, sizeof path, rel);
    if (arm64_cpio_lookup(arm64_initrd_start,
                          (size_t)(arm64_initrd_end - arm64_initrd_start),
                          path, &mode, &data, &len) < 0)
        return sysret_err(ENOENT);
    if ((mode & ARM64_S_IFMT) != ARM64_S_IFLNK)
        return sysret_err(EINVAL);
    n = len < (size_t)regs->x[3] ? len : (size_t)regs->x[3];
    if (copy_to_user((void *)(uintptr_t)regs->x[2], data, n) < 0)
        return sysret_err(EFAULT);
    return n;
}

static uint64_t sys_sendfile(struct el0_regs *regs)
{
    int out = (int)regs->x[0];
    int in = (int)regs->x[1];
    uint64_t offp = regs->x[2];
    uint64_t count = regs->x[3];
    uint64_t total = 0;
    size_t pos;

    if (out != 1 && out != 2)
        return sysret_err(EBADF);
    if (in < 0 || in >= MAX_FD || fds()[in].kind != FD_FILE)
        return sysret_err(EBADF);
    pos = fds()[in].pos;
    if (offp) {
        uint64_t off;

        if (copy_from_user(&off, (const void *)(uintptr_t)offp, 8) < 0)
            return sysret_err(EFAULT);
        pos = (size_t)off;
    }
    if (pos >= fds()[in].len)
        return 0;
    if (!count || count > fds()[in].len - pos)
        count = fds()[in].len - pos;
    while (total < count) {
        char tmp[128];
        size_t chunk = (size_t)(count - total);

        if (chunk > sizeof tmp)
            chunk = sizeof tmp;
        memcpy(tmp, fds()[in].data + pos + (size_t)total, chunk);
        for (size_t i = 0; i < chunk; i++) {
            if (tmp[i] == '\n')
                uart_putc('\r');
            uart_putc(tmp[i]);
        }
        total += chunk;
    }
    pos += (size_t)total;
    if (offp) {
        uint64_t off = pos;

        if (copy_to_user((void *)(uintptr_t)offp, &off, 8) < 0)
            return sysret_err(EFAULT);
    } else {
        fds()[in].pos = pos;
    }
    return total;
}

static uint64_t sys_brk(struct el0_regs *regs)
{
    uint64_t req = regs->x[0];

    uint64_t *brk = arm64_current() ? &arm64_current()->brk : &s_user_brk;

    if (req == 0)
        return *brk;
    if (arm64_user_range_ok((uintptr_t)req, 1) < 0)
        return *brk;
    *brk = req;
    s_user_brk = req;
    return *brk;
}

static uint64_t sys_mmap(struct el0_regs *regs)
{
    uint64_t addr = regs->x[0];
    uint64_t len = regs->x[1];
    uint64_t flags = regs->x[3];

    if (!len)
        return sysret_err(EINVAL);
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!addr) {
        uint64_t *bump = arm64_current() ? &arm64_current()->mmap_bump : &s_mmap_bump;

        if (*bump + len < *bump || *bump + len > ARM64_MMAP_END)
            return sysret_err(ENOMEM);
        addr = *bump;
        *bump += len;
        s_mmap_bump = *bump;
    } else if (arm64_user_range_ok((uintptr_t)addr, (size_t)len) < 0) {
        return sysret_err(EINVAL);
    }
    if (flags & MAP_ANONYMOUS)
        memset((void *)(uintptr_t)addr, 0, (size_t)len);
    else {
        int fd = (int)regs->x[4];
        uint64_t off = regs->x[5];

        memset((void *)(uintptr_t)addr, 0, (size_t)len);
        if (fd >= 0 && fd < MAX_FD && fds()[fd].kind == FD_FILE &&
            off < fds()[fd].len) {
            size_t n = fds()[fd].len - (size_t)off;

            if (n > (size_t)len)
                n = (size_t)len;
            memcpy((void *)(uintptr_t)addr, fds()[fd].data + (size_t)off, n);
        }
    }
    return addr;
}

#ifndef MYOS_VERSION
#define MYOS_VERSION "unknown"
#endif
static uint64_t sys_uname(struct el0_regs *regs)
{
    static const char uts[6][65] = {
        "Linux", "myos", "6.1.0-MyOS", "#1 MyOS " MYOS_VERSION, "aarch64", "(none)",
    };

    if (copy_to_user((void *)(uintptr_t)regs->x[0], uts, sizeof uts) < 0)
        return sysret_err(EFAULT);
    return 0;
}

static uint64_t sys_ioctl(struct el0_regs *regs)
{
    uint32_t cmd = (uint32_t)regs->x[1];
    struct { uint16_t row, col, x, y; } ws = { 24, 80, 0, 0 };

    if ((int)regs->x[0] > 2)
        return sysret_err(EBADF);
    if (cmd == TIOCGWINSZ) {
        if (copy_to_user((void *)(uintptr_t)regs->x[2], &ws, sizeof ws) < 0)
            return sysret_err(EFAULT);
        return 0;
    }
    if (cmd == TCGETS || cmd == TCSETS) {
        if (cmd == TCSETS &&
            copy_from_user(&cons_tty, (void *)(uintptr_t)regs->x[2],
                           sizeof cons_tty) < 0)
            return sysret_err(EFAULT);
        if (cmd == TCGETS &&
            copy_to_user((void *)(uintptr_t)regs->x[2], &cons_tty,
                         sizeof cons_tty) < 0)
            return sysret_err(EFAULT);
        return 0;
    }
    if (cmd == TIOCGPGRP || cmd == TIOCSPGRP) {
        int pg = arm64_current() ? arm64_current()->pid : 1;

        if (cmd == TIOCGPGRP &&
            copy_to_user((void *)(uintptr_t)regs->x[2], &pg, 4) < 0)
            return sysret_err(EFAULT);
        return 0;
    }
    return 0;
}

static uint64_t sys_clock_gettime(struct el0_regs *regs)
{
    struct { int64_t sec, nsec; } ts;
    uint32_t ms = timer_ms();

    ts.sec = ms / 1000u;
    ts.nsec = (int64_t)(ms % 1000u) * 1000000;
    if (copy_to_user((void *)(uintptr_t)regs->x[1], &ts, sizeof ts) < 0)
        return sysret_err(EFAULT);
    return 0;
}

static uint64_t sys_getcwd(struct el0_regs *regs)
{
    const char *root = arm64_current() && arm64_current()->cwd[0]
                           ? arm64_current()->cwd
                           : "/";
    size_t n = (size_t)regs->x[1];

    size_t need = strlen(root) + 1;

    if (n < need)
        return sysret_err(EINVAL);
    if (copy_to_user((void *)(uintptr_t)regs->x[0], root, need) < 0)
        return sysret_err(EFAULT);
    return regs->x[0];
}

#define POLLIN 0x0001u

static uint64_t sys_ppoll(struct el0_regs *regs)
{
    struct pollfd {
        int32_t fd;
        int16_t events;
        int16_t revents;
    } pf;
    int nfds = (int)regs->x[1];
    uint64_t tmo_p = regs->x[2];
    int instant = 0;
    int i;
    uint64_t ready;

    if (nfds <= 0 || nfds > 32)
        return sysret_err(EINVAL);
    if (tmo_p) {
        struct {
            int64_t sec;
            int64_t nsec;
        } ts;

        if (copy_from_user(&ts, (void *)(uintptr_t)tmo_p, sizeof ts) < 0)
            return sysret_err(EFAULT);
        instant = (ts.sec == 0 && ts.nsec == 0);
    }
    for (;;) {
        ready = 0;
        for (i = 0; i < nfds; i++) {
            int rev = 0;

            if (copy_from_user(&pf, (void *)(uintptr_t)(regs->x[0] +
                                                        (uint64_t)i * sizeof pf),
                               sizeof pf) < 0)
                return sysret_err(EFAULT);
            pf.revents = 0;
            if (pf.fd >= 0 && pf.fd < MAX_FD && (pf.events & POLLIN)) {
                if (pf.fd <= 2 && fds()[pf.fd].kind == FD_CONS &&
                    uart_readable())
                    rev = POLLIN;
                if (fds()[pf.fd].kind == FD_PIPE_R && fds()[pf.fd].pipe &&
                    fds()[pf.fd].pipe->n)
                    rev = POLLIN;
            }
            pf.revents = (int16_t)rev;
            if (rev)
                ready++;
            if (copy_to_user((void *)(uintptr_t)(regs->x[0] +
                                                 (uint64_t)i * sizeof pf),
                             &pf, sizeof pf) < 0)
                return sysret_err(EFAULT);
        }
        if (ready)
            return ready;
        if (instant || tmo_p)
            return 0;
        __asm__ volatile("wfe");
    }
}

static uint64_t sys_getrandom(struct el0_regs *regs)
{
    uint8_t tmp[64];
    size_t n = (size_t)regs->x[1];
    size_t i;

    if (n > sizeof tmp)
        n = sizeof tmp;
    for (i = 0; i < n; i++)
        tmp[i] = (uint8_t)(timer_ms() + (uint32_t)i);
    if (copy_to_user((void *)(uintptr_t)regs->x[0], tmp, n) < 0)
        return sysret_err(EFAULT);
    return n;
}

uint64_t syscall_dispatch_el0(uint64_t nr, struct el0_regs *regs)
{
    switch (nr) {
    case __NR_io_setup:
    case __NR_io_destroy:
        return 0;
    case __NR_write:
        return sys_write(regs);
    case __NR_writev:
        return sys_writev(regs);
    case __NR_fcntl: {
        int cmd = (int)regs->x[1];
        int old = (int)regs->x[0];
        int minfd = (int)regs->x[2];
        int n;

        if (cmd != 0 && cmd != 1030) /* F_DUPFD / F_DUPFD_CLOEXEC */
            return 0;
        if (old < 0 || old >= MAX_FD || fds()[old].kind == FD_NONE)
            return sysret_err(EBADF);
        if (minfd < 0)
            minfd = 0;
        for (n = minfd; n < MAX_FD; n++) {
            if (fds()[n].kind == FD_NONE) {
                fds()[n] = fds()[old];
                if (fds()[n].pipe) {
                    if (fds()[n].kind == FD_PIPE_R)
                        fds()[n].pipe->rrefs++;
                    if (fds()[n].kind == FD_PIPE_W)
                        fds()[n].pipe->wrefs++;
                }
                return (uint64_t)n;
            }
        }
        return sysret_err(24);
    }
    case __NR_dup:
        regs->x[1] = (uint64_t)-1;
        return arm64_sys_dup3(regs);
    case __NR_dup3:
        return arm64_sys_dup3(regs);
    case __NR_pipe2:
        return arm64_sys_pipe2(regs);
    case __NR_clone:
        return arm64_sys_clone(regs);
    case __NR_execve:
        return arm64_sys_execve(regs);
    case __NR_wait4:
        return arm64_sys_wait4(regs);
    case __NR_getdents64:
        return sys_getdents64(regs);
    case __NR_chdir:
        return sys_chdir(regs);
    case __NR_readlinkat:
        return sys_readlinkat(regs);
    case __NR_pselect6:
    case __NR_ppoll:
        return sys_ppoll(regs);
    case __NR_nanosleep:
    case __NR_clock_nanosleep:
        return 0;
    case __NR_fstat:
        return sys_fstat_fd((int)regs->x[0], regs->x[1]);
    case __NR_newfstatat:
        return sys_newfstatat(regs);
    case __NR_faccessat:
    case __NR_faccessat2:
        return sys_faccessat(regs);
    case __NR_read:
        return sys_read(regs);
    case __NR_pread64:
        return sys_pread64(regs);
    case __NR_pwrite64:
        return sysret_err(EINVAL);
    case __NR_close:
        return sys_close(regs);
    case __NR_lseek:
        return sys_lseek(regs);
    case __NR_openat:
        return sys_openat(regs);
    case __NR_sendfile:
        return sys_sendfile(regs);
    case __NR_ioctl:
        return sys_ioctl(regs);
    case __NR_getcwd:
        return sys_getcwd(regs);
    case __NR_uname:
        return sys_uname(regs);
    case __NR_clock_gettime:
        return sys_clock_gettime(regs);
    case __NR_getrandom:
        return sys_getrandom(regs);
    case __NR_set_tid_address:
        if (arm64_current())
            arm64_current()->tid_addr = regs->x[0];
        return arm64_current() ? (uint64_t)arm64_current()->pid : 1;
    case __NR_set_robust_list:
    case __NR_rt_sigaction:
    case __NR_rt_sigprocmask:
    case __NR_prctl:
    case __NR_mprotect:
    case __NR_munmap:
        return 0;
    case __NR_getpid:
    case __NR_gettid:
        return arm64_current() ? (uint64_t)arm64_current()->pid : 1;
    case __NR_getppid:
        return arm64_current() ? (uint64_t)arm64_current()->ppid : 0;
    case __NR_setpgid:
    case __NR_setsid:
    case __NR_kill:
    case __NR_sync:
    case __NR_setfsuid:
    case __NR_setfsgid:
    case __NR_prlimit64:
        return 0;
    case __NR_getpgid:
    case __NR_getsid:
        return arm64_current() ? (uint64_t)arm64_current()->pid : 1;
    case __NR_getrlimit: {
        uint64_t inf[2] = { ~0ull, ~0ull };

        if (copy_to_user((void *)(uintptr_t)regs->x[1], inf, sizeof inf) < 0)
            return sysret_err(EFAULT);
        return 0;
    }
    case __NR_readv:
        return 0;
    case __NR_getuid:
    case __NR_geteuid:
    case __NR_getgid:
    case __NR_getegid:
        return 0;
    case __NR_brk:
        return sys_brk(regs);
    case __NR_mmap:
        return sys_mmap(regs);
    case __NR_exit:
    case __NR_exit_group:
        s_el0_pass = 1;
        return arm64_sys_exit(regs, (int)regs->x[0]);
    case SYS_myos_el0_ping:
        if (regs->x[0] == 0x44415441ULL) {
            kprintf("  EL0: data access ok\n");
            s_el0_pass = 1;
            if (s_el1_resume_pc)
                arm64_return_to_el1(s_el1_resume_pc);
            return 0;
        }
        kprintf("  EL0 svc: ping arg0=%llu\n", (unsigned long long)regs->x[0]);
        kprintf("  EL0: svc probe ok\n");
        s_el0_pass = 1;
        return 0;
    case SYS_myos_el0_done:
        if (s_el1_resume_pc)
            arm64_return_to_el1(s_el1_resume_pc);
        return 0;
    default: {
        static uint64_t seen[16];
        static int nseen;

        if (nseen < 16) {
            int i, dup = 0;

            for (i = 0; i < nseen; i++)
                if (seen[i] == nr)
                    dup = 1;
            if (!dup) {
                seen[nseen++] = nr;
                kprintf("  EL0 svc: unknown nr %llu\n", (unsigned long long)nr);
            }
        }
        return sysret_err(ENOSYS);
    }
    }
}

void sync_exception_el0(uint64_t esr, uint64_t elr, struct el0_regs *regs)
{
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);

    if (ec == ESR_EC_SVC64) {
        uint64_t nr = regs->x[8];
        uint64_t ret;

        arm64_proc_on_svc(regs, elr);
        ret = syscall_dispatch_el0(nr, regs);
        arm64_proc_after_svc(regs, ret);
        return;
    }
    (void)elr;
    {
        uint64_t far;

        __asm__ volatile("mrs %0, far_el1" : "=r"(far));
        uart_puts("\n*** sync exception EL0 *** ESR=0x");
        uart_puthex((unsigned long)esr);
        uart_puts(" ELR=0x");
        uart_puthex((unsigned long)elr);
        uart_puts(" FAR=0x");
        uart_puthex((unsigned long)far);
        uart_puts("\n");
    }
    for (;;)
        __asm__ volatile("wfe");
}

extern void user_el0_stub(void);
extern void user_write_test(void);
extern void user_el0_databtest(void);
extern void arm64_enter_el0(void *entry, void *sp_el0);

static void el0_start_user(void *entry);

void el0_run_probe(void)
{
    el0_start_user((void *)user_el0_stub);
}

void el0_run_write_test(void)
{
    el0_start_user((void *)user_write_test);
}

void el0_run_databtest(void)
{
    el0_start_user((void *)user_el0_databtest);
}

int el0_run_initrd(void)
{
    size_t sz = (size_t)(arm64_initrd_end - arm64_initrd_start);
    int n, rc;
    struct arm64_proc *p;

    n = arm64_cpio_file_count(arm64_initrd_start, sz);
    kprintf("  initramfs: %d files, %u bytes\n", n, (unsigned)sz);
    kprintf("  initramfs: /init\n");
    arm64_proc_init_pid1((void *)(uintptr_t)ARM64_USER_IMAGE_VA,
                         (void *)(uintptr_t)(ARM64_USER_STACK_TOP - 16),
                         ARM64_USER_IMAGE_VA + 0x1000);
    rc = arm64_do_exec("/init", 0, 0);
    if (rc < 0) {
        kprintf("  initramfs: exec /init failed %d\n", rc);
        return -1;
    }
    p = arm64_current();
    p->execed = 0; /* first enter is via eret, not after_svc */
    s_user_entry = (void *)(uintptr_t)p->elr;
    s_user_sp = (void *)(uintptr_t)p->sp_el0;
    el0_set_user_brk(p->brk);
    kprintf("  EL0: /init entry=%p sp=%p brk=0x%llx\n", s_user_entry, s_user_sp,
            (unsigned long long)p->brk);
    return 0;
}

static void el0_start_user(void *entry)
{
    /* Stack in the EL0-mapped 2 MiB at ARM64_USER_MIN. */
    s_user_sp = (void *)(uintptr_t)(ARM64_USER_MIN + 0x10000ULL - 16ULL);
    s_user_entry = entry;
    kprintf("  EL0: entering user at %p sp=%p\n", s_user_entry, s_user_sp);
}

void el0_enter_user(void)
{
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb sy");
    arm64_enter_el0(s_user_entry, s_user_sp);
}
