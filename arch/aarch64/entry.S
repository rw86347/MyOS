/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * entry.S — first instructions in the ARM64 kernel (QEMU virt / future Apple)
 *
 * Boot convention (QEMU -kernel, Linux-compatible):
 *   x0 = physical address of the device tree blob (FDT)
 *   MMU off, EL1 (or EL2 then EL1 — virt drops us in EL1)
 *
 * Apple Silicon (step 15) will use the same register convention once m1n1
 * (or similar) hands off; only the DTB and UART addresses differ.
 */
    .section .text.entry
    .global _start
    .type   _start, %function

    .extern kernel_main
    .extern __bss_start, __bss_end
#ifdef M1N1_SIMULATE_X0
    .extern apple_harness_dtb_start
#endif

_start:
    /* QEMU may enter at EL2; drop to EL1 before touching EL1-only state. */
    mrs     x10, CurrentEL
    and     x10, x10, #0xc
    cmp     x10, #8
    b.ne    .Lel1_ok
    mov     x10, #(1 << 31)         /* HCR_EL2.RW */
    msr     hcr_el2, x10
    adr     x10, .Lel1_ok
    msr     elr_el2, x10
    mov     x10, #0x3c5             /* EL1h, DAIF masked */
    msr     spsr_el2, x10
    eret
.Lel1_ok:
    /* Preserve FDT pointer for early C (memory map, chosen console, …). */
    mov     x19, x0
#ifdef M1N1_SIMULATE_X0
    /* CI: QEMU -kernel ELF often leaves x0=0; m1n1 on hardware passes DTB in x0. */
    cbnz    x19, .Ldtb_ok
    ldr     x19, =apple_harness_dtb_start
.Ldtb_ok:
#endif

    /* Clear .bss — QEMU does not zero it for -kernel ELF loads. */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x2, x1, x0
    cbz     x2, .Lbss_done
.Lbss:
    str     xzr, [x0], #8
    subs    x2, x2, #8
    b.hi    .Lbss
.Lbss_done:

    /* Fixed stack in RAM (see linker.ld __stack_top). */
    ldr     x0, =__stack_top
    mov     sp, x0

    mov     x0, x19
    bl      kernel_main

.Lhang:
    wfe
    b       .Lhang
