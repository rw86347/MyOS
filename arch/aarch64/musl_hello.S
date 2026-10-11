/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
    .section .rodata.musl
    .align  8
    .global musl_hello_blob
    .global musl_hello_blob_end
musl_hello_blob:
    .incbin MUSL_HELLO_ELF
musl_hello_blob_end:
