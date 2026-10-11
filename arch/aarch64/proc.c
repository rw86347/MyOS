/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * proc.c — ARM64 bring-up processes: fork/vfork, execve, wait, pipes
 *
 * One CPU. A process that waits retries the syscall (ELR -= 4) after the
 * child or writer runs. Address spaces: vfork shares; fork copies; exec
 * unshares and loads a new image (BusyBox or musl ld.so + bash).
 */
#include "proc.h"

#include <kprintf.h>
#include <memlayout.h>
#include <string.h>

#include "cpio.h"
#include "elf64_load.h"
#include "uaccess.h"

#define EPERM   1
#define ENOENT  2
#define ECHILD  10
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define EINVAL  22
#define ENOTDIR 20
#define EMFILE  24
#define ENOEXEC 8

#define CLONE_VM              0x00000100u
#define CLONE_VFORK           0x00004000u
#define CLONE_PARENT_SETTID   0x00100000u
#define CLONE_CHILD_CLEARTID  0x00200000u
#define CLONE_CHILD_SETTID    0x01000000u

#define WNOHANG 1
#define MAX_ARGV 16

static struct arm64_proc procs[ARM64_MAX_PROC];
static struct arm64_proc *cur;
static struct arm64_pipe pipes[4];
static struct arm64_fd boot_fds[ARM64_MAX_FD] = {
    [0] = { FD_CONS, 0, 0, 0, { 0 }, 0 },
    [1] = { FD_CONS, 0, 0, 0, { 0 }, 0 },
    [2] = { FD_CONS, 0, 0, 0, { 0 }, 0 },
};

static uint64_t err(int e)
{
    return (uint64_t)(-(long)e);
}

struct arm64_proc *arm64_current(void)
{
    return cur;
}

struct arm64_fd *arm64_cur_fds(void)
{
    return cur ? cur->fds : boot_fds;
}

static size_t initrd_size(void)
{
    return (size_t)(arm64_initrd_end - arm64_initrd_start);
}

static void pick_next(void)
{
    int start = cur ? (int)(cur - procs) : 0;
    int i;

    for (i = 1; i <= ARM64_MAX_PROC; i++) {
        struct arm64_proc *p = &procs[(start + i) % ARM64_MAX_PROC];

        if (p->state == PROC_RUN) {
            cur = p;
            paging_as_activate(&p->as);
            return;
        }
    }
}

void arm64_proc_on_svc(struct el0_regs *regs, uint64_t elr)
{
    uint64_t spsr, sp;

    if (!cur)
        return;
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp));
    memcpy(&cur->regs, regs, sizeof *regs);
    cur->elr = elr;
    cur->spsr = spsr;
    cur->sp_el0 = sp;
}

void arm64_proc_after_svc(struct el0_regs *regs, uint64_t ret)
{
    if (cur && cur->state == PROC_RUN && !cur->execed)
        cur->regs.x[0] = ret;
    if (!cur || cur->state != PROC_RUN)
        pick_next();
    if (!cur)
        return;
    if (cur->execed) {
        memset(regs, 0, sizeof *regs);
        cur->execed = 0;
    } else {
        memcpy(regs, &cur->regs, sizeof *regs);
    }
    __asm__ volatile("msr elr_el1, %0" :: "r"(cur->elr));
    __asm__ volatile("msr spsr_el1, %0" :: "r"(cur->spsr));
    __asm__ volatile("msr sp_el0, %0" :: "r"(cur->sp_el0));
    paging_as_activate(&cur->as);
}

void arm64_block_retry(void)
{
    if (!cur)
        return;
    cur->state = PROC_WAIT;
    cur->elr -= 4;
}

static void wake_pid(int pid)
{
    int i;

    for (i = 0; i < ARM64_MAX_PROC; i++) {
        if (procs[i].pid == pid &&
            (procs[i].state == PROC_WAIT || procs[i].state == PROC_VFORK_WAIT))
            procs[i].state = PROC_RUN;
    }
}

static struct arm64_proc *alloc_proc(void)
{
    int i;

    for (i = 0; i < ARM64_MAX_PROC; i++) {
        if (procs[i].state == PROC_FREE)
            return &procs[i];
    }
    return 0;
}

int arm64_proc_init_pid1(void *entry, void *sp, uint64_t brk)
{
    memset(procs, 0, sizeof procs);
    cur = &procs[0];
    cur->pid = 1;
    cur->ppid = 0;
    cur->state = PROC_RUN;
    cur->elr = (uint64_t)(uintptr_t)entry;
    cur->spsr = 0x3c0;
    cur->sp_el0 = (uint64_t)(uintptr_t)sp;
    cur->brk = brk;
    cur->mmap_bump = ARM64_MMAP_BASE;
    cur->fds[0].kind = FD_CONS;
    cur->fds[1].kind = FD_CONS;
    cur->fds[2].kind = FD_CONS;
    memcpy(cur->cwd, "/", 2);
    paging_as_wrap_boot(&cur->as);
    return 0;
}

static int join_rel(char *curpath, const char *tgt, size_t tlen)
{
    char t[256];
    char *slash;
    size_t n;

    if (tlen >= sizeof t)
        return -1;
    memcpy(t, tgt, tlen);
    t[tlen] = 0;
    if (t[0] == '/') {
        n = strlen(t);
        if (n >= 256)
            return -1;
        memcpy(curpath, t, n + 1);
        return 0;
    }
    slash = strrchr(curpath, '/');
    if (!slash)
        return -1;
    slash[1] = 0;
    n = strlen(curpath);
    if (n + strlen(t) >= 256)
        return -1;
    memcpy(curpath + n, t, strlen(t) + 1);
    return 0;
}

static void trim_dot_path(char *path)
{
    size_t n;

    if (!path)
        return;
    if (strcmp(path, ".") == 0 || strcmp(path, "/.") == 0) {
        strcpy(path, "/");
        return;
    }
    n = strlen(path);
    while (n >= 2 && path[n - 2] == '/' && path[n - 1] == '.') {
        path[n - 2] = '\0';
        n -= 2;
        if (n == 0) {
            path[0] = '/';
            path[1] = '\0';
            return;
        }
    }
}

void arm64_abs_path(char *out, size_t outsz, const char *rel)
{
    const char *cwd = cur && cur->cwd[0] ? cur->cwd : "/";

    if (!out || !outsz)
        return;
    if (!rel || !rel[0] || (rel[0] == '.' && rel[1] == '\0')) {
        size_t n = strlen(cwd);

        if (n >= outsz)
            n = outsz - 1;
        memcpy(out, cwd, n);
        out[n] = 0;
        return;
    }
    if (rel[0] == '/') {
        size_t n = strlen(rel);

        if (n >= outsz)
            n = outsz - 1;
        memcpy(out, rel, n);
        out[n] = 0;
        trim_dot_path(out);
        return;
    }
    {
        size_t n = strlen(cwd);
        size_t r = strlen(rel);

        if (n + 1 + r >= outsz)
            r = outsz - n - 2;
        memcpy(out, cwd, n);
        if (n && out[n - 1] != '/')
            out[n++] = '/';
        memcpy(out + n, rel, r);
        out[n + r] = 0;
        trim_dot_path(out);
    }
}

int arm64_resolve_path(const char *path, uint32_t *mode, const void **data,
                       size_t *len)
{
    char curpath[256];
    int hops;

    if (!path || !path[0] || strlen(path) >= sizeof curpath)
        return -1;
    memcpy(curpath, path, strlen(path) + 1);
    for (hops = 0; hops < 8; hops++) {
        if (arm64_cpio_lookup(arm64_initrd_start, initrd_size(), curpath, mode,
                              data, len) < 0)
            return -1;
        if ((*mode & ARM64_S_IFMT) != ARM64_S_IFLNK)
            return 0;
        if (join_rel(curpath, (const char *)*data, *len) < 0)
            return -1;
    }
    return -1;
}

static int parse_shebang(const uint8_t *data, size_t len, char *interp,
                         size_t isz, char *arg, size_t asz)
{
    size_t i = 2, a, b;

    if (len < 4 || data[0] != '#' || data[1] != '!')
        return -1;
    while (i < len && (data[i] == ' ' || data[i] == '\t'))
        i++;
    a = i;
    while (i < len && data[i] != ' ' && data[i] != '\t' && data[i] != '\n')
        i++;
    if (i == a || i - a >= isz)
        return -1;
    memcpy(interp, data + a, i - a);
    interp[i - a] = 0;
    arg[0] = 0;
    while (i < len && (data[i] == ' ' || data[i] == '\t'))
        i++;
    if (i < len && data[i] != '\n') {
        b = i;
        while (i < len && data[i] != ' ' && data[i] != '\t' && data[i] != '\n')
            i++;
        if (i > b && i - b < asz) {
            memcpy(arg, data + b, i - b);
            arg[i - b] = 0;
        }
    }
    return 0;
}

int arm64_do_exec(const char *path, const char *const *argv,
                  const char *const *envp)
{
    const void *data = 0, *idata = 0;
    size_t len = 0, ilen = 0;
    uint32_t mode = 0, imode = 0;
    char interp[128], sarg[64], ipath[128];
    const char *nargv[MAX_ARGV + 1];
    struct arm64_elf_loaded loaded;
    int narg = 0;
    static const char *const def_env[] = {
        "PATH=/bin:/usr/bin", "HOME=/", "TERM=linux", "USER=root", 0
    };

    if (arm64_resolve_path(path, &mode, &data, &len) < 0)
        return -ENOENT;
    if ((mode & ARM64_S_IFMT) == ARM64_S_IFDIR)
        return -EACCES;
    if (!arm64_elf_is_image(data, len)) {
        if (parse_shebang(data, len, interp, sizeof interp, sarg, sizeof sarg) < 0)
            return -ENOEXEC;
        nargv[narg++] = interp;
        if (sarg[0])
            nargv[narg++] = sarg;
        nargv[narg++] = path;
        if (argv) {
            int i = 1;

            while (argv[i] && narg < MAX_ARGV)
                nargv[narg++] = argv[i++];
        }
        nargv[narg] = 0;
        if (arm64_resolve_path(interp, &mode, &data, &len) < 0)
            return -ENOENT;
        path = interp;
        argv = nargv;
    }
    if (!arm64_elf_is_image(data, len))
        return -ENOEXEC;
    if (cur && cur->shared_mm) {
        struct arm64_as fresh;
        int i;

        if (paging_as_fresh(&fresh) < 0)
            return -ENOMEM;
        cur->as = fresh;
        cur->shared_mm = 0;
        for (i = 0; i < ARM64_MAX_PROC; i++) {
            if (procs[i].state != PROC_FREE && procs[i].pid != cur->pid)
                procs[i].shared_mm = 0;
        }
        paging_as_activate(&cur->as);
    }
    ipath[0] = 0;
    if (arm64_elf_interp(data, len, ipath, sizeof ipath) == 0) {
        if (arm64_resolve_path(ipath, &imode, &idata, &ilen) < 0)
            return -ENOENT;
    }
    if (!envp)
        envp = def_env;
    if (arm64_elf_load_exec(data, len, argv, envp, idata, ilen, path, &loaded) < 0)
        return -ENOEXEC;
    if (cur) {
        cur->elr = (uint64_t)(uintptr_t)loaded.entry;
        cur->sp_el0 = (uint64_t)(uintptr_t)loaded.user_sp;
        cur->brk = loaded.brk;
        cur->mmap_bump = ARM64_MMAP_BASE;
        cur->execed = 1;
        memset(&cur->regs, 0, sizeof cur->regs);
        if (cur->ppid)
            wake_pid(cur->ppid);
    }
    return 0;
}

static int copy_strvec(uint64_t uptr, char store[][128], const char *out[],
                       int max)
{
    int n = 0;

    if (!uptr) {
        out[0] = 0;
        return 0;
    }
    for (; n < max; n++) {
        uint64_t p = 0;
        size_t i;

        if (copy_from_user(&p, (const void *)(uintptr_t)(uptr + (uint64_t)n * 8), 8) < 0)
            return -1;
        if (!p) {
            out[n] = 0;
            return n;
        }
        memset(store[n], 0, 128);
        for (i = 0; i < 127; i++) {
            if (copy_from_user(store[n] + i, (const char *)(uintptr_t)p + i, 1) < 0)
                return -1;
            if (store[n][i] == 0)
                break;
        }
        store[n][127] = 0;
        out[n] = store[n];
    }
    out[n] = 0;
    return n;
}

uint64_t arm64_sys_execve(struct el0_regs *regs)
{
    char path[256];
    char abuf[16][128];
    char ebuf[8][128];
    const char *argv[17];
    const char *envp[9];
    size_t i;
    int rc;

    memset(path, 0, sizeof path);
    for (i = 0; i < sizeof path - 1; i++) {
        if (copy_from_user(path + i, (const char *)(uintptr_t)regs->x[0] + i, 1) < 0)
            return err(EFAULT);
        if (path[i] == 0)
            break;
    }
    if (!path[0])
        return err(ENOENT);
    if (copy_strvec(regs->x[1], abuf, argv, 16) < 0)
        return err(EFAULT);
    if (copy_strvec(regs->x[2], ebuf, envp, 8) < 0)
        return err(EFAULT);
    rc = arm64_do_exec(path, argv, envp[0] ? envp : 0);
    if (rc < 0)
        return err(-rc);
    return 0;
}

static void copy_fds(struct arm64_proc *dst, struct arm64_proc *src)
{
    int i;

    memcpy(dst->fds, src->fds, sizeof src->fds);
    for (i = 0; i < ARM64_MAX_FD; i++) {
        if (dst->fds[i].pipe) {
            if (dst->fds[i].kind == FD_PIPE_R)
                dst->fds[i].pipe->rrefs++;
            if (dst->fds[i].kind == FD_PIPE_W)
                dst->fds[i].pipe->wrefs++;
        }
    }
}

uint64_t arm64_sys_clone(struct el0_regs *regs)
{
    uint32_t flags = (uint32_t)regs->x[0];
    uint64_t stack = regs->x[1];
    struct arm64_proc *child;
    static int next_pid = 2;

    if (!cur)
        return err(EINVAL);
    child = alloc_proc();
    if (!child)
        return err(ENOMEM);
    memset(child, 0, sizeof *child);
    child->pid = next_pid++;
    child->ppid = cur->pid;
    child->state = PROC_RUN;
    child->elr = cur->elr;
    child->spsr = cur->spsr;
    child->sp_el0 = stack ? stack : cur->sp_el0;
    child->brk = cur->brk;
    child->mmap_bump = cur->mmap_bump;
    child->tid_addr = regs->x[4];
    memcpy(&child->regs, &cur->regs, sizeof child->regs);
    child->regs.x[0] = 0;
    copy_fds(child, cur);
    memcpy(child->cwd, cur->cwd, sizeof child->cwd);
    if (flags & CLONE_VM) {
        memcpy(&child->as, &cur->as, sizeof child->as);
        child->shared_mm = 1;
        cur->shared_mm = 1;
    } else if (paging_as_fork(&child->as, &cur->as) < 0) {
        child->state = PROC_FREE;
        return err(ENOMEM);
    }
    if (flags & CLONE_VFORK)
        cur->state = PROC_VFORK_WAIT;
    if (flags & CLONE_PARENT_SETTID && regs->x[2]) {
        uint32_t pid = (uint32_t)child->pid;

        copy_to_user((void *)(uintptr_t)regs->x[2], &pid, 4);
    }
    if (flags & CLONE_CHILD_SETTID && regs->x[4]) {
        uint32_t pid = (uint32_t)child->pid;

        /* child's address space — write after we switch, or via identity. */
        copy_to_user((void *)(uintptr_t)regs->x[4], &pid, 4);
    }
    return (uint64_t)child->pid;
}

uint64_t arm64_sys_wait4(struct el0_regs *regs)
{
    int want = (int)regs->x[0];
    int opts = (int)regs->x[2];
    int i, found = 0;

    if (!cur)
        return err(EINVAL);
    for (i = 0; i < ARM64_MAX_PROC; i++) {
        struct arm64_proc *p = &procs[i];

        if (p->state == PROC_FREE || p->ppid != cur->pid)
            continue;
        found = 1;
        if (p->state != PROC_ZOMBIE)
            continue;
        if (want > 0 && p->pid != want)
            continue;
        if (regs->x[1]) {
            int st = p->exit_code << 8;

            if (copy_to_user((void *)(uintptr_t)regs->x[1], &st, 4) < 0)
                return err(EFAULT);
        }
        {
            int pid = p->pid;

            memset(p, 0, sizeof *p);
            p->state = PROC_FREE;
            return (uint64_t)pid;
        }
    }
    if (!found)
        return err(ECHILD);
    if (opts & WNOHANG)
        return 0;
    arm64_block_retry();
    return 0;
}

uint64_t arm64_sys_exit(struct el0_regs *regs, int code)
{
    int i;

    (void)regs;
    if (!cur)
        for (;;)
            __asm__ volatile("wfe");
    if (cur->tid_addr)
        copy_to_user((void *)(uintptr_t)cur->tid_addr, "\0\0\0\0", 4);
    for (i = 0; i < ARM64_MAX_FD; i++) {
        if (cur->fds[i].pipe) {
            if (cur->fds[i].kind == FD_PIPE_R && cur->fds[i].pipe->rrefs)
                cur->fds[i].pipe->rrefs--;
            if (cur->fds[i].kind == FD_PIPE_W && cur->fds[i].pipe->wrefs)
                cur->fds[i].pipe->wrefs--;
        }
        cur->fds[i].kind = FD_NONE;
    }
    if (!cur->shared_mm)
        paging_as_release(&cur->as);
    cur->exit_code = code & 0xff;
    cur->state = PROC_ZOMBIE;
    if (cur->ppid)
        wake_pid(cur->ppid);
    else {
        for (;;)
            __asm__ volatile("wfe");
    }
    pick_next();
    return 0;
}

static struct arm64_pipe *alloc_pipe(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (!pipes[i].rrefs && !pipes[i].wrefs) {
            memset(&pipes[i], 0, sizeof pipes[i]);
            pipes[i].rrefs = 1;
            pipes[i].wrefs = 1;
            return &pipes[i];
        }
    }
    return 0;
}

static int alloc_fd_from(int lo)
{
    int i;

    for (i = lo; i < ARM64_MAX_FD; i++) {
        if (cur->fds[i].kind == FD_NONE)
            return i;
    }
    return -1;
}

uint64_t arm64_sys_pipe2(struct el0_regs *regs)
{
    struct arm64_pipe *p;
    int r, w;
    int fds[2];

    if (!cur)
        return err(EINVAL);
    p = alloc_pipe();
    if (!p)
        return err(ENOMEM);
    r = alloc_fd_from(3);
    w = r >= 0 ? alloc_fd_from(r + 1) : -1;
    if (r < 0 || w < 0) {
        p->rrefs = p->wrefs = 0;
        return err(EMFILE);
    }
    cur->fds[r].kind = FD_PIPE_R;
    cur->fds[r].pipe = p;
    cur->fds[w].kind = FD_PIPE_W;
    cur->fds[w].pipe = p;
    fds[0] = r;
    fds[1] = w;
    if (copy_to_user((void *)(uintptr_t)regs->x[0], fds, sizeof fds) < 0)
        return err(EFAULT);
    return 0;
}

void arm64_pipe_wake(struct arm64_pipe *pipe)
{
    int i, j;

    if (!pipe)
        return;
    for (i = 0; i < ARM64_MAX_PROC; i++) {
        if (procs[i].state != PROC_WAIT)
            continue;
        for (j = 0; j < ARM64_MAX_FD; j++) {
            if (procs[i].fds[j].pipe == pipe) {
                procs[i].state = PROC_RUN;
                break;
            }
        }
    }
}

uint64_t arm64_sys_dup3(struct el0_regs *regs)
{
    int old = (int)regs->x[0];
    int newfd = (int)regs->x[1];

    if (!cur || old < 0 || old >= ARM64_MAX_FD || cur->fds[old].kind == FD_NONE)
        return err(9);
    if (newfd < 0) {
        newfd = alloc_fd_from(0);
        if (newfd < 0)
            return err(EMFILE);
    }
    if (newfd >= ARM64_MAX_FD)
        return err(9);
    if (newfd == old)
        return (uint64_t)newfd;
    cur->fds[newfd] = cur->fds[old];
    if (cur->fds[newfd].pipe) {
        if (cur->fds[newfd].kind == FD_PIPE_R)
            cur->fds[newfd].pipe->rrefs++;
        if (cur->fds[newfd].kind == FD_PIPE_W)
            cur->fds[newfd].pipe->wrefs++;
    }
    return (uint64_t)newfd;
}
