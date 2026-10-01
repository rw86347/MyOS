/*
 * bootinfo.h — what the MBR leaves behind for the kernel
 *
 * The bootloader (arch/i386/boot/mbr.asm) stores the BIOS E820 memory map
 * at physical address BOOTINFO_PHYS:
 *   u32 count, then `count` 24-byte entries.
 * Keep in sync with BOOTINFO / E820_MAX in mbr.asm.
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
    uint32_t count;
    struct e820_entry entries[E820_MAX];
} __attribute__((packed));

#endif
