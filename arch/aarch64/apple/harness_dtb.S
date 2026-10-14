/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* Apple harness DTB for QEMU CI when ELF -kernel leaves x0 zero (m1n1 sets x0 on hardware). */
    .section .rodata.apple_harness_dtb, "a", %progbits
    .global apple_harness_dtb_start, apple_harness_dtb_end
    .align 8
apple_harness_dtb_start:
    .incbin ARM64_HARNESS_DTB_PATH
apple_harness_dtb_end:
