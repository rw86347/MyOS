/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_APPLE_M1N1_H
#define ARCH_AARCH64_APPLE_M1N1_H

#include <stdint.h>

/* Physical load address for kernel.bin / ELF (see arch/aarch64/linker.ld). */
#define ARM64_M1N1_KERNEL_LOAD 0x40100000u

void arm64_boot_report(uint64_t dtb_from_x0, uintptr_t dtb_used);

#endif
