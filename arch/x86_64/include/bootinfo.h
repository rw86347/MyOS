/*
 * bootinfo.h — what the 64-bit UEFI loader leaves at 0x1000
 *
 * Same layout as arch/i386/include/bootinfo.h so pmm, initrd, and simplefb
 * share one description. initrd and framebuffer use full 64-bit physical
 * addresses where needed.
 */
#ifndef ARCH_X86_64_BOOTINFO_H
#define ARCH_X86_64_BOOTINFO_H

#include <stdint.h>
#include <types.h>

#define BOOTINFO_PHYS     0x1000ull
#define FONT_PHYS         0x2000ull
#define E820_MAX          32
#define BOOTINFO_FB_MAGIC 0x3242464Du  /* 'MFB2' */
#define BOOTINFO_MAGIC    0x34584F4Du  /* 'MOX4' — loader sanity (optional) */

#define E820_USABLE       1
#define E820_RESERVED     2
#define E820_ACPI_RECLAIM 3
#define E820_ACPI_NVS     4
#define E820_BAD          5

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

struct bootinfo {
    uint64_t initrd_phys;
    uint32_t initrd_size;
    uint32_t count;
    struct e820_entry entries[E820_MAX];
    uint64_t font_phys;
    uint32_t fb_magic;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
    uint32_t fb_format;             /* 0 = XRGB, 1 = RGBX */
    uint64_t fb_phys;               /* full GOP address */
    uint32_t loader_magic;          /* BOOTINFO_MAGIC if loader filled GOP */
} __attribute__((packed));

static inline phys_addr_t bootinfo_fb_phys(const struct bootinfo *bi)
{
    return bi->fb_phys;
}

#endif
