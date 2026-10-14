/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_I386_SYSENTER_H
#define ARCH_I386_SYSENTER_H

#include <stdint.h>

/* Set up the sysenter MSRs and the vsyscall page (if the CPU has sysenter). */
void sysenter_init(void);

/* i386's vsyscall page; x86_64 programs use `syscall`, so: always 0 (no
   AT_SYSINFO). */
uint32_t vsyscall_map(uint64_t at);
static inline void vsyscall_switch(uint64_t stack_top) { (void)stack_top; }

#endif
