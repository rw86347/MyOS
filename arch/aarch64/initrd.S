/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
    .section .rodata.initrd, "a", %progbits
    .align  8
    .global arm64_initrd_start, arm64_initrd_end
arm64_initrd_start:
    .incbin ARM64_INITRD_PATH
arm64_initrd_end:
