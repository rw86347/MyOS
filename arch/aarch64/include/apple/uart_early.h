/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_APPLE_UART_EARLY_H
#define ARCH_AARCH64_APPLE_UART_EARLY_H

#include <stdint.h>

void arm64_uart_init_from_dtb(uintptr_t dtb_phys);
/* Early console reg from DTB (apple,s5l-uart / apple,uart / PL011). Returns 0 if found. */
int arm64_early_console_mmio(uintptr_t dtb_phys, uint64_t *base_out, uint64_t *size_out);

#endif
