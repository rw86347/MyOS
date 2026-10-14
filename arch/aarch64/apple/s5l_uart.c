/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * apple,s5l-uart — early TX/RX (Samsung-derived Apple UART, not PL011).
 * Offsets match Linux/u-boot PORT_S5L; bootloader/m1n1 often leaves UART on.
 */
#include "apple/s5l_uart.h"

#include <stdint.h>

#define S5L_UFSTAT 0x018u
#define S5L_UTXH   0x020u
#define S5L_URXH   0x024u
#define S5L_ULCON  0x000u
#define S5L_UCON   0x004u
#define S5L_UFCON  0x008u
#define S5L_UBRDIV 0x028u
#define S5L_UFRAC  0x02cu

#define S5L_TX_FIFO_FULL (1u << 9)

static uintptr_t s_base;

static inline void wr32(uintptr_t off, uint32_t v)
{
    *(volatile uint32_t *)(s_base + off) = v;
}

static inline uint32_t rd32(uintptr_t off)
{
    return *(volatile uint32_t *)(s_base + off);
}

void apple_s5l_uart_bind(uintptr_t phys)
{
    if (phys)
        s_base = phys;
}

void apple_s5l_uart_init(void)
{
    if (!s_base)
        return;
    /* Minimal bring-up if m1n1 did not configure the port (115200-ish @ 24 MHz ref). */
    wr32(S5L_UFCON, 0x07u);
    wr32(S5L_ULCON, 0x03u);
    wr32(S5L_UBRDIV, 13u);
    wr32(S5L_UFRAC, 1u);
    wr32(S5L_UCON, (1u << 2) | (1u << 0)); /* IRQ mode fields + enable */
}

void apple_s5l_uart_putc(char c)
{
    if (!s_base)
        return;
    while (rd32(S5L_UFSTAT) & S5L_TX_FIFO_FULL)
        ;
    wr32(S5L_UTXH, (uint32_t)(unsigned char)c);
}

int apple_s5l_uart_readable(void)
{
    uint32_t st;

    if (!s_base)
        return 0;
    st = rd32(S5L_UFSTAT);
    return (st & 0xfu) != 0u;
}

int apple_s5l_uart_getc(void)
{
    if (!apple_s5l_uart_readable())
        return -1;
    return (int)(rd32(S5L_URXH) & 0xffu);
}
