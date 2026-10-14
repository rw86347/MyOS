/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * syscall.c — set up the `syscall` instruction (musl's way into the kernel)
 *
 * Four MSRs:
 *   EFER.SCE   turns the instruction on (without it, `syscall` is #UD)
 *   LSTAR      where it jumps: syscall_entry_asm
 *   STAR       the selectors. syscall loads CS = STAR[47:32] and SS = that
 *              + 8; sysret loads SS = STAR[63:48] + 8 and CS = that + 16
 *              (the GDT order in gdt.c exists for this).
 *   SFMASK     RFLAGS bits cleared on entry: IF (interrupts stay off, as with
 *              int 0x80 on i386), and TF, DF, AC, NT — the kernel starts with
 *              clean flags whatever the program left in them.
 */
#include <cpu.h>
#include <gdt.h>
#include <stdint.h>

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE   (1u << 0)

#define FL_TF (1u << 8)
#define FL_IF (1u << 9)
#define FL_DF (1u << 10)
#define FL_NT (1u << 14)
#define FL_AC (1u << 18)

extern void syscall_entry_asm(void);

void syscall_init(void)
{
    uint64_t sysret_base = GDT_USER_DATA - 8;           /* 0x1B: RPL 3 included */
    wrmsr(MSR_STAR, (sysret_base << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry_asm);
    wrmsr(MSR_SFMASK, FL_IF | FL_TF | FL_DF | FL_NT | FL_AC);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void sysenter_init(void)
{
    syscall_init();
}

uint32_t vsyscall_map(uint64_t at)
{
    (void)at;
    return 0;
}
