/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * user_stub.S — tiny EL0 code linked in the kernel image (identity-mapped RAM)
 */
    .section .text.user, "ax", %progbits
    .global user_el0_stub
    .type   user_el0_stub, %function

user_el0_stub:
    mov     x8, #240
    mov     x0, #42
    svc     #0
.Lhang:
    wfi
    b       .Lhang

    .global arm64_enter_el0
    .type   arm64_enter_el0, %function
/* void arm64_enter_el0(void *entry, void *sp_el0); */
arm64_enter_el0:
    msr     sp_el0, x1
    msr     elr_el1, x0
    mov     x2, #0x3c0
    msr     spsr_el1, x2
    eret

    .global arm64_return_to_el1
    .type   arm64_return_to_el1, %function
/* void arm64_return_to_el1(void *el1_pc); */
arm64_return_to_el1:
    ldr     x2, =__stack_top
    mov     sp, x2
    msr     elr_el1, x0
    mov     x1, #0x5
    msr     spsr_el1, x1
    eret
