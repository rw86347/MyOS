/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * power.c — restart, halt, power off.
 *
 * Restart: the keyboard controller (the 8042) has a wire to the CPU's reset
 * pin; command 0xFE pulses it. That's how PCs have rebooted since the AT.
 * If nothing happens, a triple fault resets the CPU (cpu.c).
 *
 * Power off: on a real PC that is ACPI. The firmware's tables (the DSDT)
 * say which number to write to the power-management port for "soft off",
 * and reading those tables needs an AML interpreter we don't have. QEMU
 * (and so UTM) is simpler: its number is 0. We only do it there — writing
 * a guess to a real board's port could put it in some other sleep state —
 * and recognise QEMU by the subsystem vendor it gives every PCI device
 * (0x1AF4, Red Hat). Anywhere else, power off = halt with a message.
 */
#include <cpu.h>
#include <io.h>
#include <kprintf.h>
#include <pci.h>
#include <power.h>
#include <proc.h>
#include <serial.h>
#include <signal.h>

#define KBC_STATUS   0x64
#define KBC_BUSY     0x02           /* the controller hasn't taken the last byte */
#define KBC_RESET    0xFE

#define QEMU_SUBVENDOR  0x1AF4
#define PM1_CNT         4           /* offset from the power-management base */
#define SLP_EN          (1u << 13)  /* "sleep now", in the type bits 10..12 */

static int cad_restart = 1;         /* Linux's default until init says */

/* Print the last words and stop interrupts: nothing runs after this. */
static void last_words(const char *msg)
{
    kprintf("%s\n", msg);
    __asm__ volatile ("cli");
    serial_flush();                 /* interrupts are off: send it all now */
}

void power_restart(void)
{
    last_words("reboot: restarting system");
    for (int i = 0; i < 100000 && (inb(KBC_STATUS) & KBC_BUSY); i++)
        ;                           /* bounded: a dead controller can't hang us */
    outb(KBC_STATUS, KBC_RESET);
    for (volatile int i = 0; i < 1000000; i++)
        ;                           /* the reset takes a moment to arrive */
    cpu_reset();
}

void power_halt(void)
{
    last_words("System halted. It is now safe to turn off the computer.");
    cpu_halt_forever();
}

/* QEMU's power-management function: PIIX4 on the "pc" machine, the ICH9
   bridge on "q35". Both keep the I/O base in config register 0x40. */
static void qemu_soft_off(void)
{
    struct pci_dev *d = pci_find(0x8086, 0x7113);
    if (!d)
        d = pci_find(0x8086, 0x2918);
    if (!d || (pci_read_config(d, 0x2C) & 0xFFFF) != QEMU_SUBVENDOR)
        return;
    uint16_t base = pci_read_config(d, 0x40) & 0xFFC0;
    if (base)
        outw(base + PM1_CNT, SLP_EN);   /* sleep type 0 = soft off */
}

void power_off(void)
{
    kprintf("reboot: power down\n");
    serial_flush();
    __asm__ volatile ("cli");
    qemu_soft_off();
    power_halt();                   /* still here: no way to switch off */
}

void power_set_ctrl_alt_del(int restart_now)
{
    cad_restart = restart_now;
}

void power_ctrl_alt_del(void)
{
    if (cad_restart)
        power_restart();            /* no sync: like Linux, it's "now" */
    struct process *init = proc_find(1);
    if (init)
        signal_send(init, SIGINT);
}
