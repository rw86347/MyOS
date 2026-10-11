/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fpu.c — the floating-point unit (x87) and SSE registers
 *
 * Besides EAX..ESP, the CPU has a second set of registers for maths:
 *   x87:  8 floating-point registers (a little stack: st0..st7), plus
 *         control/status words — the original 8087 coprocessor, built in
 *         since the 486
 *   SSE:  8 128-bit registers xmm0..xmm7, plus MXCSR (Pentium III and later)
 * A program doing printf("%f") or any float maths uses them. When we switch
 * threads, they must be saved and restored like the normal registers —
 * otherwise one program's numbers leak into another's.
 *
 * FXSAVE / FXRSTOR save and load all of it (512 bytes) in one instruction.
 * CPUs without FXSAVE (before the Pentium II) only have x87, which
 * FNSAVE / FRSTOR handle (108 bytes).
 *
 * Who needs saving: only USER threads. The kernel is compiled with
 * -mgeneral-regs-only, so kernel code never touches these registers and
 * kernel threads don't need to save anything.
 *
 * We save eagerly on every switch away from a user thread. The old trick of
 * saving "lazily" (only when another thread actually uses the FPU) was
 * dropped by Linux in 2018: it leaked register contents between processes
 * (the "LazyFP" attack).
 */
#include <cpu.h>
#include <fpu.h>
#include <kprintf.h>
#include <string.h>

#define CR0_MP  (1 << 1)    /* "monitor coprocessor": needed with TS */
#define CR0_EM  (1 << 2)    /* "emulate": if set, every FPU instruction faults */
#define CR0_TS  (1 << 3)    /* "task switched": FPU instructions fault (lazy FPU) */
#define CR0_NE  (1 << 5)    /* report FPU errors as exception 16, not an IRQ */
#define CR4_OSFXSR     (1 << 9)     /* OS saves SSE state with FXSAVE: SSE allowed */
#define CR4_OSXMMEXCPT (1 << 10)    /* OS handles SSE exceptions (exception 19) */

static int has_fxsr;

/* The state of a freshly reset FPU: every new user thread starts from it. */
static uint8_t clean_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

void fpu_init(void)
{
    has_fxsr = cpu_has(CPU_FXSR);

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    if (has_fxsr) {
        uint32_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_OSFXSR;
        if (cpu_has(CPU_SSE))
            cr4 |= CR4_OSXMMEXCPT;
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    }

    __asm__ volatile ("fninit");        /* reset the x87 unit */
    fpu_save(clean_state);
    fpu_restore(clean_state);           /* (FNSAVE also resets; undo that) */

    klog("FPU: x87%s, saved with %s\n", cpu_has(CPU_SSE) ? " + SSE" : "",
            has_fxsr ? "FXSAVE" : "FNSAVE");
}

void fpu_save(void *state)
{
    if (has_fxsr)
        __asm__ volatile ("fxsave (%0)" : : "r"(state) : "memory");
    else
        __asm__ volatile ("fnsave (%0)" : : "r"(state) : "memory");
}

void fpu_restore(const void *state)
{
    if (has_fxsr)
        __asm__ volatile ("fxrstor (%0)" : : "r"(state) : "memory");
    else
        __asm__ volatile ("frstor (%0)" : : "r"(state) : "memory");
}

void fpu_clean_state(void *state)
{
    memcpy(state, clean_state, FPU_STATE_SIZE);
}
