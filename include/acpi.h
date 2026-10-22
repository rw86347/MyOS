/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* The firmware's ACPI tables, for power off (drivers/acpi.c). */

/* At boot, before the first process: find the PM1 control ports and _S5's
   sleep types, and say what was found. */
void acpi_init(void);

/* Is ACPI power off known (acpi_init found everything)? */
int acpi_can_power_off(void);

/* Interrupts off: switch the machine off. Returns (-1) only if it didn't. */
int acpi_power_off(void);

/* Every table the firmware lists (the XSDT's, and the DSDT the FADT points
   at), kept by acpi_init. The i'th: its name as Linux shows it in
   /sys/firmware/acpi/tables ("DSDT", or "SSDT1", "SSDT2"... when there is
   more than one), its bytes and length. NULL past the last. */
const void *acpi_table_at(int i, const char **name, uint32_t *len);
/* The n'th (from 1) table with this signature, or NULL. */
const void *acpi_table_find(const char *sig, int n, uint32_t *len);
/* Where the RSDP is (0: no ACPI), and memory acpi_init mapped already:
   the address of [phys, phys + len) if it did, else NULL. */
uint64_t acpi_rsdp(void);
const void *acpi_mapped(uint64_t phys, uint32_t len);

/* The AML interpreter (acpi_os.c): its thread runs everything below. */
void acpi_start(void);                  /* after the threads and PCI */
void acpi_poke(void);                   /* wake the thread (an interrupt may) */
void acpi_ec_init(void);                /* acpi_ec.c */
void acpi_ec_poll(void);                /* send the EC's queries, if it has news */
void battery_init(void);                /* battery.c: find the batteries and chargers */
uint32_t battery_poll(void);            /* read what is due; ms until the next (0: none) */
void backlight_init(void);              /* backlight.c: find the panel's _BCL and _BCM */
void backlight_poll(void);              /* run _BCM for a level that was written */

/* For /sys/class/backlight/acpi_video0 (sysfs.c): the highest level (-1: no
   panel the firmware can dim), the one asked for last, the one set, and a
   new one (0, or -errno; set by the acpi thread soon after). */
int backlight_max(void);
int backlight_get(void);
int backlight_actual(void);
int backlight_set(int level);

/* For /sys/class/power_supply (sysfs.c): the i'th battery or charger, its
   name ("BAT0", "AC"; NULL until it can be shown), attribute k's name (NULL: it hasn't that one; k <
   power_supply_attrs()), and its text now, or -errno. */
int power_supply_count(void);
const char *power_supply_name(int i);
int power_supply_attrs(void);
const char *power_supply_attr(int i, int k);
int power_supply_show(int i, int k, char *out, int size);

#endif
