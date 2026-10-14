/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_I386_SYSENTER_H
#define ARCH_I386_SYSENTER_H

#include <stdint.h>

/* Set up the sysenter MSRs and the vsyscall page (if the CPU has sysenter). */
void sysenter_init(void);

/* Map the vsyscall page (sysentry.asm) into the current program, at `at`:
   the page right above its stack (mm->stack_top), so that it moves with
   the stack (ASLR) and shares its page table. Returns the address to pass
   as AT_SYSINFO, or 0 if this CPU has no sysenter (int 0x80 only). */
uint32_t vsyscall_map(uint32_t at);

/* Where sysexit returns: .ret in the current program's vsyscall page,
   which sysentry.asm reads on every sysenter (one load, where a fixed page
   had a constant). Set when the address space changes (mm_switch) and
   when exec maps the page (vsyscall_map). */
extern uint32_t sysexit_eip;
extern const char vsyscall_ret_offset[];        /* sysentry.asm */
static inline void vsyscall_switch(uint32_t stack_top)
{
    sysexit_eip = stack_top + (uint32_t)vsyscall_ret_offset;
}

#endif
