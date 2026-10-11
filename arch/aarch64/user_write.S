/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * user_write.S — EL0 program: Linux write(1, msg, len) then wfi
 */
    .section .user.rodata, "a", %progbits
    .align  1
    .global user_write_msg
user_write_msg:
    .ascii  "user write: hi from EL0\n"
user_write_msg_end:
    .set user_write_len, user_write_msg_end - user_write_msg

    .section .user.text, "ax", %progbits
    .global user_write_test
    .type   user_write_test, %function

    .global user_el0_databtest
    .type   user_el0_databtest, %function
user_el0_databtest:
    adr     x1, user_write_msg
    ldr     x0, [x1]
    str     x0, [x1]
    mov     x8, #240
    movz    x0, #0x5441
    movk    x0, #0x4441, lsl #16  /* 'DATA' — databtest ok */
    svc     #0
    mov     x8, #241
    svc     #0
    b       .Lhang

user_write_test:
    mov     x8, #64
    mov     x0, #1
    adr     x1, user_write_msg
    ldr     x9, [x1]
    mov     x2, #user_write_len
    svc     #0
.Lhang:
    wfi
    b       .Lhang
