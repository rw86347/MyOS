/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_UART_H
#define ARCH_AARCH64_UART_H

#include <stddef.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(unsigned long v);
void uart_printf(const char *fmt, ...);

#endif
