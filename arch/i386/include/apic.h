/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_APIC_H
#define ARCH_APIC_H

#include <stdint.h>

/*
 * The local APIC (apic.c), for MSI and MSI-X. Switched on the first time a
 * driver asks for an MSI IRQ; until then the 8259 does everything.
 */

/* Switch the local APIC on (once). 0, or -1 if the CPU has none or the
   BIOS turned it off (then there is no MSI: devices use the 8259). */
int lapic_enable(void);

/* End of an MSI interrupt (interrupts.c, after the handlers). */
void lapic_eoi(void);

/* n consecutive MSI IRQ numbers (16-31): the first, or -1 if used up. */
int msi_irq_alloc(int n);

/* The message a device writes to raise `irq` on this CPU: the address
   (high half always 0 here) and the data. */
void msi_compose(int irq, uint32_t *addr_lo, uint32_t *addr_hi, uint32_t *data);

#endif
