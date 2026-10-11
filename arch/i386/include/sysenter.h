/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_I386_SYSENTER_H
#define ARCH_I386_SYSENTER_H

#include <stdint.h>

/* Where the vsyscall page (sysentry.asm) sits in every program: the top
   user page, just above the stack. Keep in sync with sysentry.asm. */
#define VSYSCALL_ADDR 0xBFFFF000u

/* Set up the sysenter MSRs and the vsyscall page (if the CPU has sysenter). */
void sysenter_init(void);

/* Map the vsyscall page into the current program. Returns the address to
   pass as AT_SYSINFO, or 0 if this CPU has no sysenter (int 0x80 only). */
uint32_t vsyscall_map(void);

#endif
