/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* Device tree linked into the kernel (QEMU ELF -kernel does not pass a DTB). */
    .section .rodata.dtb, "a", %progbits
    .global myos_dtb_start, myos_dtb_end
    .align 8
myos_dtb_start:
    .incbin MYOS_DTB_PATH
myos_dtb_end:
