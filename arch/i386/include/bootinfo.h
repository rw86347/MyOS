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

#define BOOTINFO_PHYS 0x1000
#define E820_MAX      32

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
} __attribute__((packed));

#endif
