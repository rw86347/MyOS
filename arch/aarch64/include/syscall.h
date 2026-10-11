/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_SYSCALL_H
#define ARCH_AARCH64_SYSCALL_H

#include <stdint.h>

/* Linux aarch64: nr in x8, args x0-x5, return x0, svc #0. */
#define ESR_EC_SVC64  0x15u

/* Linux aarch64 syscall numbers (include/uapi/asm-generic/unistd.h). */
#define __NR_io_setup         0u
#define __NR_io_destroy       1u
#define __NR_getcwd           17u
#define __NR_dup              23u
#define __NR_dup3             24u
#define __NR_fcntl            25u
#define __NR_ioctl            29u
#define __NR_faccessat        48u
#define __NR_faccessat2       439u
#define __NR_chdir            49u
#define __NR_openat           56u
#define __NR_close            57u
#define __NR_pipe2            59u
#define __NR_getdents64       61u
#define __NR_lseek            62u
#define __NR_read             63u
#define __NR_readv            65u
#define __NR_write            64u
#define __NR_pread64          67u
#define __NR_pwrite64         68u
#define __NR_writev           66u
#define __NR_sendfile         71u
#define __NR_pselect6         72u
#define __NR_ppoll            73u
#define __NR_readlinkat       78u
#define __NR_newfstatat       79u
#define __NR_fstat            80u
#define __NR_exit             93u
#define __NR_exit_group       94u
#define __NR_set_tid_address  96u
#define __NR_nanosleep        101u
#define __NR_clock_nanosleep  115u
#define __NR_kill             129u
#define __NR_set_robust_list  99u
#define __NR_clock_gettime    113u
#define __NR_rt_sigaction     134u
#define __NR_rt_sigprocmask   135u
#define __NR_sync             81u
#define __NR_setfsuid         151u
#define __NR_setfsgid         152u
#define __NR_getpgid          155u
#define __NR_getsid           156u
#define __NR_setpgid          154u
#define __NR_getrlimit        163u
#define __NR_prlimit64        261u
#define __NR_uname            160u
#define __NR_setsid           157u
#define __NR_prctl            167u
#define __NR_getpid           172u
#define __NR_getppid          173u
#define __NR_getuid           174u
#define __NR_geteuid          175u
#define __NR_getgid           176u
#define __NR_getegid          177u
#define __NR_gettid           178u
#define __NR_brk              214u
#define __NR_munmap           215u
#define __NR_clone            220u
#define __NR_execve           221u
#define __NR_mmap             222u
#define __NR_mprotect         226u
#define __NR_wait4            260u
#define __NR_getrandom        278u

#define SYS_myos_el0_ping  240u /* bring-up: arg0 echoed in log */
#define SYS_myos_el0_done  241u /* sets pass flag, user may wfi */

struct el0_regs {
    uint64_t x[31]; /* x0-x30 */
};

int  el0_test_passed(void);
void el0_set_resume(void *el1_pc);
void el0_set_user_brk(uint64_t brk);
void el0_run_probe(void);
void el0_run_write_test(void);
void el0_run_databtest(void);
int  el0_run_initrd(void);
void el0_enter_user(void);

uint64_t syscall_dispatch_el0(uint64_t nr, struct el0_regs *regs);

#endif
