/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * exceptions.S — EL1 vector table (step 14a)
 *
 * Only synchronous exceptions from the current EL are handled for now; the
 * handler prints ESR/ELR/FAR on the UART and halts.
 */
    .section .text.vectors, "ax", %progbits
    .align 11
    .global vector_table

    .macro ventry handler
    .align 7
    b       \handler
    .endm

vector_table:
    ventry  hang
    ventry  hang
    ventry  hang
    ventry  hang
    ventry  sync_el1_sp0
    ventry  irq_el1
    ventry  hang
    ventry  hang
    ventry  sync_el0
    ventry  irq_el0
    ventry  hang
    ventry  hang
    ventry  sync_el1
    ventry  hang
    ventry  hang
    ventry  hang

    .section .bss
    .align 12
    .global el0_exc_stack_top
el0_exc_stack:
    .skip 4096
el0_exc_stack_top:

    .section .text.entry

    .macro save_regs
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    stp     x6, x7, [sp, #-16]!
    stp     x8, x9, [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x19, [sp, #-16]!
    stp     x20, x21, [sp, #-16]!
    stp     x22, x23, [sp, #-16]!
    stp     x24, x25, [sp, #-16]!
    stp     x26, x27, [sp, #-16]!
    stp     x28, x29, [sp, #-16]!
    str     x30, [sp, #-8]!
    .endm

    .macro restore_regs
    ldr     x30, [sp], #8
    ldp     x28, x29, [sp], #16
    ldp     x26, x27, [sp], #16
    ldp     x24, x25, [sp], #16
    ldp     x22, x23, [sp], #16
    ldp     x20, x21, [sp], #16
    ldp     x18, x19, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8, x9, [sp], #16
    ldp     x6, x7, [sp], #16
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
    .endm

    .extern sync_exception_el1
    .extern sync_exception_el0
    .extern irq_exception_el1

sync_el0:
    msr     daifset, #0xf
    sub     sp, sp, #256            /* x0-x30 + pad; x[i] at offset 8*i */
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mov     x19, sp
    ldr     x20, =el0_exc_stack_top
    mov     sp, x20
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mov     x2, x19
    bl      sync_exception_el0
    /* Frame may now be another process (fork/wait). Do not restore x8. */
    mov     sp, x19
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #256
    /* Keep IRQs masked until eret restores the user SPSR. daifclr here
     * let a timer IRQ clobber ELR_EL1 and the following eret looped. */
    eret

sync_el1_sp0:
sync_el1:
    msr     daifset, #4
    save_regs
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      sync_exception_el1
    restore_regs
    eret

irq_el1:
    msr     daifset, #0xf
    save_regs
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #-16]!
    bl      irq_exception_el1
    ldp     x0, x1, [sp], #16
    msr     elr_el1, x0
    msr     spsr_el1, x1
    restore_regs
    eret

irq_el0:
    msr     daifset, #0xf
    save_regs
    mov     x19, sp
    ldr     x20, =el0_exc_stack_top
    mov     sp, x20
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #-16]!
    bl      irq_exception_el1
    ldp     x0, x1, [sp], #16
    msr     elr_el1, x0
    msr     spsr_el1, x1
    mov     sp, x19
    restore_regs
    eret

hang:
    wfe
    b       hang

    .global exceptions_init
    .extern vector_table
exceptions_init:
    adr     x0, vector_table
    msr     vbar_el1, x0
    /* musl crt / libc use FP/SIMD; trap-none at EL0+EL1 (CPACR_EL1.FPEN=11). */
    mov     x0, #(3 << 20)
    msr     CPACR_EL1, x0
    isb
    ret

    .global arm64_irq_enable
    .type   arm64_irq_enable, %function
arm64_irq_enable:
    msr     daifclr, #2
    isb
    ret
