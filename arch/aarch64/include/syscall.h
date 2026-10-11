/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_SYSCALL_H
#define ARCH_AARCH64_SYSCALL_H

#include <stdint.h>

/* Linux aarch64: nr in x8, args x0-x5, return x0, svc #0. */
#define ESR_EC_SVC64  0x15u

/* Linux aarch64 syscall numbers (arch/arm64/include/uapi/asm/unistd.h). */
#define __NR_write         64u
#define __NR_exit          93u
#define __NR_exit_group    94u

#define SYS_myos_el0_ping  240u /* bring-up: arg0 echoed in log */
#define SYS_myos_el0_done  241u /* sets pass flag, user may wfi */

struct el0_regs {
    uint64_t x[31]; /* x0-x30 */
};

int  el0_test_passed(void);
void el0_set_resume(void *el1_pc);
void el0_run_probe(void);
void el0_run_write_test(void);
void el0_run_databtest(void);
void el0_run_musl_hello(void);
void el0_enter_user(void);

uint64_t syscall_dispatch_el0(uint64_t nr, struct el0_regs *regs);

#endif
