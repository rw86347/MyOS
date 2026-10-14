/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_UART_H
#define ARCH_AARCH64_UART_H

#include <stddef.h>
#include <stdint.h>

void uart_set_base(uintptr_t phys);
void uart_use_apple_s5l(uintptr_t phys);
void uart_init(void);
void uart_putc(char c);
int  uart_readable(void);
int  uart_getc(void); /* -1 if the RX FIFO is empty */
void uart_puts(const char *s);
void uart_puthex(unsigned long v);
void uart_printf(const char *fmt, ...);

#endif
