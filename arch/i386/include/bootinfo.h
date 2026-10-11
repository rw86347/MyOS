/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * bootinfo.h — what the bootloader leaves behind for the kernel
 *
 * Stage 2 (arch/i386/boot/stage2.asm) writes this at physical address
 * BOOTINFO_PHYS: where it loaded the initramfs, and the BIOS E820 memory map.
 * Keep in sync with BOOTINFO / E820_MAX in arch/i386/boot/layout.inc.
 */
#ifndef ARCH_I386_BOOTINFO_H
#define ARCH_I386_BOOTINFO_H

#include <stdint.h>
#include <types.h>

#define BOOTINFO_PHYS 0x1000
#define E820_MAX      32
#define BOOTINFO_FB_MAGIC 0x3242464Du   /* 'MFB2': GOP/simplefb fields are valid */

/* E820 region types */
#define E820_USABLE        1    /* normal RAM, free for us */
#define E820_RESERVED      2    /* BIOS, devices: never touch */
#define E820_ACPI_RECLAIM  3    /* ACPI tables: usable after reading them */
#define E820_ACPI_NVS      4    /* ACPI non-volatile storage: never touch */
#define E820_BAD           5    /* defective RAM */

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

struct bootinfo {
    uint32_t initrd_phys;           /* physical address of the initramfs */
    uint32_t initrd_size;           /* its size in bytes (0 = none) */
    uint32_t count;                 /* number of E820 entries */
    struct e820_entry entries[E820_MAX];
    uint32_t font_phys;             /* the BIOS's 8x16 font, 4 KiB (0 = none) */
    /* UEFI GOP (0 on a BIOS boot: stage 2 writes zeros). */
    uint32_t fb_magic;
    uint32_t fb_phys;
    uint32_t fb_pitch;              /* bytes per line */
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
    uint32_t fb_format;             /* 0 = XRGB8888 (R at 16), 1 = RGBX (R at 0) */
    uint32_t fb_phys_hi;            /* physical bits 63:32 (0 if below 4 GiB) */
} __attribute__((packed));

static inline phys_addr_t bootinfo_fb_phys(const struct bootinfo *bi)
{
    return (phys_addr_t)bi->fb_phys | ((phys_addr_t)bi->fb_phys_hi << 32);
}

_Static_assert(__builtin_offsetof(struct bootinfo, font_phys) == 12 + E820_MAX * 24,
               "BOOTINFO_FONT in layout.inc");
_Static_assert(__builtin_offsetof(struct bootinfo, fb_magic) == 16 + E820_MAX * 24,
               "fb_magic follows font_phys");

#endif
