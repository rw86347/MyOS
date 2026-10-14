/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_APPLE_S5L_UART_H
#define ARCH_AARCH64_APPLE_S5L_UART_H

#include <stdint.h>

/* MacBook Air (M1) serial0 — Asahi t8103 DT (J313). */
#define ARM64_APPLE_M1_AIR_UART_PHYS 0x235200000ull

void apple_s5l_uart_bind(uintptr_t phys);
void apple_s5l_uart_init(void);
void apple_s5l_uart_putc(char c);
int  apple_s5l_uart_readable(void);
int  apple_s5l_uart_getc(void);

#endif
