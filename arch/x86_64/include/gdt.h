/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

/* Segment selectors (see gdt.c for the table and why this order). */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x23            /* entry 4, RPL 3 */
#define GDT_USER_CODE   0x2B            /* entry 5, RPL 3 */
#define GDT_TSS         0x30            /* entries 6-7 */

#ifndef __ASSEMBLER__
#include <stdint.h>

/* Install the kernel's GDT and reload all segment registers. */
void gdt_init(void);

/* The stack the CPU switches to when a user program enters the kernel
   (syscall, interrupt, fault). Set on every thread switch. */
void tss_set_kernel_stack(uint64_t rsp0);

/* Thread-local storage. On x86_64 it is not a segment but the FS base
   (an MSR, set with arch_prctl): thread->tls[0] holds it, and loading a
   thread's "TLS" writes that MSR. The i386 names stay, for shared code. */
#define GDT_TLS_FIRST   8
#define GDT_TLS_ENTRIES 3
void     gdt_load_tls(const uint64_t tls[GDT_TLS_ENTRIES]);
uint64_t gdt_tls_descriptor(uint32_t base, uint32_t limit, uint32_t flags);

/* Whether a segment register can be loaded with this selector on the way back
   to ring 3 without faulting in the kernel first. See gdt.c. */
int      gdt_user_seg_ok(uint32_t sel);
#endif

#endif
