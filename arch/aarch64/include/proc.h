/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_PROC_H
#define ARCH_AARCH64_PROC_H

#include <stddef.h>
#include <stdint.h>

#include "paging.h"
#include "syscall.h"

#define ARM64_MAX_PROC 4
#define ARM64_MAX_FD   32
#define ARM64_PIPE_CAP 1024

#define PROC_FREE       0
#define PROC_RUN        1
#define PROC_WAIT       2
#define PROC_VFORK_WAIT 3
#define PROC_ZOMBIE     4

#define FD_NONE   0
#define FD_CONS   1
#define FD_FILE   2
#define FD_DIR    3
#define FD_PIPE_R 4
#define FD_PIPE_W 5

struct arm64_pipe {
    char buf[ARM64_PIPE_CAP];
    size_t r, n;
    int rrefs, wrefs;
};

struct arm64_fd {
    int kind;
    const uint8_t *data;
    size_t len;
    size_t pos;
    char prefix[64];
    struct arm64_pipe *pipe;
};

struct arm64_proc {
    int pid, ppid, state, exit_code;
    int execed;
    struct el0_regs regs;
    uint64_t elr, spsr, sp_el0;
    uint64_t brk, mmap_bump, tid_addr;
    struct arm64_fd fds[ARM64_MAX_FD];
    struct arm64_as as;
    int shared_mm;
    char cwd[64];
    uint64_t sa_handler[32];
};

struct arm64_proc *arm64_current(void);
struct arm64_fd   *arm64_cur_fds(void);

void arm64_proc_on_svc(struct el0_regs *regs, uint64_t elr);
void arm64_proc_after_svc(struct el0_regs *regs, uint64_t ret);

int  arm64_proc_init_pid1(void *entry, void *sp, uint64_t brk);
void arm64_block_retry(void);

uint64_t arm64_sys_clone(struct el0_regs *regs);
uint64_t arm64_sys_execve(struct el0_regs *regs);
uint64_t arm64_sys_wait4(struct el0_regs *regs);
uint64_t arm64_sys_exit(struct el0_regs *regs, int code);
uint64_t arm64_sys_pipe2(struct el0_regs *regs);
uint64_t arm64_sys_dup3(struct el0_regs *regs);
int      arm64_do_exec(const char *path, const char *const *argv,
                       const char *const *envp);
int      arm64_resolve_path(const char *path, uint32_t *mode, const void **data,
                            size_t *len);
void     arm64_pipe_wake(struct arm64_pipe *pipe);
void     arm64_abs_path(char *out, size_t outsz, const char *rel);

extern char arm64_initrd_start[];
extern char arm64_initrd_end[];

#endif
