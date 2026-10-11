/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * uart.c — PL011 early console (QEMU virt @ 0x09000000)
 *
 * Works with the MMU off (identity map). Later paging must map this device.
 */
#include "uart.h"

#include <stdarg.h>
#include <stdint.h>

#define UART_BASE 0x09000000u
#define UART_DR   0x00u
#define UART_FR   0x18u
#define UART_CR   0x30u
#define UART_FR_TXFF (1u << 5)
#define UART_FR_RXFE (1u << 4)
#define UART_CR_UARTEN (1u << 0)
#define UART_CR_TXE    (1u << 8)
#define UART_CR_RXE    (1u << 9)

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

void uart_init(void)
{
    mmio_write32(UART_BASE + UART_CR, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
}

int uart_readable(void)
{
    return !(mmio_read32(UART_BASE + UART_FR) & UART_FR_RXFE);
}

int uart_getc(void)
{
    if (!uart_readable())
        return -1;
    return (int)(mmio_read32(UART_BASE + UART_DR) & 0xff);
}

void uart_putc(char c)
{
    while (mmio_read32(UART_BASE + UART_FR) & UART_FR_TXFF)
        ;
    mmio_write32(UART_BASE + UART_DR, (uint32_t)(unsigned char)c);
}

void uart_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
    }
}

void uart_puthex(unsigned long v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[16];
    int i = 0;

    if (!v) {
        uart_putc('0');
        return;
    }
    while (v && i < 16) {
        buf[i++] = hex[v & 0xf];
        v >>= 4;
    }
    while (i > 0)
        uart_putc(buf[--i]);
}

static void putu(unsigned long v)
{
    char buf[20];
    int i = 0;

    if (!v) {
        uart_putc('0');
        return;
    }
    while (v) {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0)
        uart_putc(buf[--i]);
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (fmt[0] != '%') {
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 's':
            uart_puts(va_arg(ap, const char *));
            break;
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 'x':
        case 'p':
            uart_puthex(va_arg(ap, unsigned long));
            break;
        case 'u':
            putu(va_arg(ap, unsigned long));
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('?');
            break;
        }
    }
    va_end(ap);
}
