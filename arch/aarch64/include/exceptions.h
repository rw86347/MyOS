/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_EXCEPTIONS_H
#define ARCH_AARCH64_EXCEPTIONS_H

#include <types.h>

void exceptions_init(void);
void sync_exception_el1(uint64_t esr, uint64_t elr, uint64_t far);
void irq_exception_el1(void);

struct el0_regs;
void sync_exception_el0(uint64_t esr, uint64_t elr, struct el0_regs *regs);

void arm64_irq_enable(void);

#endif
