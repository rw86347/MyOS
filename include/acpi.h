/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ACPI_H
#define ACPI_H

/* The firmware's ACPI tables, for power off (drivers/acpi.c). */

/* At boot, before the first process: find the PM1 control ports and _S5's
   sleep types, and say what was found. */
void acpi_init(void);

/* Is ACPI power off known (acpi_init found everything)? */
int acpi_can_power_off(void);

/* Interrupts off: switch the machine off. Returns (-1) only if it didn't. */
int acpi_power_off(void);

#endif
