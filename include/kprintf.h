/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef KPRINTF_H
#define KPRINTF_H

/*
 * kprintf — the kernel's printf. Output goes to both the VGA screen and the
 * serial port. Supports: %s %c %d %u %x %p %%, with an optional width and
 * '0' padding (e.g. %8u, %08x) and '-' for left-align (e.g. %-12s).
 * Use %lu / %lx for uint32_t (it is "unsigned long" with i686-elf-gcc).
 */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Same, but to the serial port only: for detail that would clutter the
   25-line screen (memory maps, debug output). */
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * The boot's layout (kernel/main.c). The screen shows only this:
 *
 *   MyOS, written by Rodger Wilson...
 *   Step 01 - Boot loader
 *   Step 02 - CPU
 *   ...
 *
 * one line per stage, a name of at most ~20 characters; a stage only debug
 * and test builds have ends in "(debug mode)". So while the kernel boots,
 * what a driver finds is klog (serial and dmesg, indented under its step)
 * and kprintf is for warnings and errors (indented, and on the screen).
 * boot_title(0) puts the first two lines on the screen only: the UEFI
 * loaders print them on the firmware's console, serial included.
 */
void boot_title(int on_serial);
void boot_step(const char *name);
void boot_end(void);                    /* the first program starts: no indent */

/* Write n bytes to the screen and the serial port, as one piece. */
#include <stddef.h>
void console_write(const char *s, size_t n);

#endif
