/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#include "exceptions.h"
#include "gic.h"
#include "arm64_timer.h"
#include "uart.h"

void irq_exception_el1(void)
{
    unsigned int irq = gic_ack();

    if (irq == GIC_INT_TIMER_EL1)
        timer_on_irq();
    else if (irq != 1023u)
        uart_puts("  IRQ: unexpected\n");

    if (irq != 1023u)
        gic_eoi(irq);
}

void sync_exception_el1(uint64_t esr, uint64_t elr, uint64_t far)
{
    uart_puts("\n*** sync exception EL1 ***\n");
    uart_puts("ESR_EL1=0x");
    uart_puthex(esr);
    uart_puts(" ELR_EL1=0x");
    uart_puthex(elr);
    uart_puts(" FAR_EL1=0x");
    uart_puthex(far);
    uart_puts("\n");
    for (;;)
        __asm__ volatile("wfe");
}
