/*
 * gdt.c — the kernel's Global Descriptor Table
 *
 * The MBR set up a temporary GDT inside its own 512 bytes (at 0x7C00),
 * memory we will want to reuse later. So the kernel builds its own.
 *
 * Reminder (see mbr.asm for the bit-level layout): in protected mode a
 * segment register holds a SELECTOR = index * 8 + RPL, where RPL is the
 * privilege level (0 = kernel, 3 = user). We use the "flat" model: every
 * segment covers all 4 GiB starting at 0. Segments then only matter for
 * one thing: the privilege level code runs at.
 *
 *   index  selector  what
 *   0      0x00      null (required, unusable)
 *   1      0x08      kernel code, ring 0
 *   2      0x10      kernel data, ring 0
 *   3      0x1B      user code,   ring 3   (0x18 | RPL 3) — used from step 5
 *   4      0x23      user data,   ring 3   (0x20 | RPL 3)
 *   (a TSS entry joins in step 5, for switching stacks on syscalls)
 */
#include <stdint.h>
#include <gdt.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;        /* present, privilege level, code/data, permissions */
    uint8_t  flags_limit;   /* high 4 bits: flags; low 4 bits: limit bits 16-19 */
    uint8_t  base_high;
} __attribute__((packed));

/* The 6-byte operand of the LGDT instruction. */
struct gdt_ptr {
    uint16_t size;          /* table size in bytes, minus 1 */
    uint32_t base;          /* address of the table */
} __attribute__((packed));

static struct gdt_entry gdt[5];

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].access      = access;
    gdt[i].flags_limit = (flags << 4) | ((limit >> 16) & 0x0F);
    gdt[i].base_high   = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
    /* access byte: 0x9A = present | ring 0 | code | executable + readable
                    0x92 = present | ring 0 | data | writable
                    0xFA / 0xF2 = the same, but ring 3 (bits 5-6 = 11)
       flags 0xC: 4 KiB granularity (limit counts pages) + 32-bit segment */
    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xC);
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC);
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xC);
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xC);

    struct gdt_ptr ptr = { sizeof gdt - 1, (uint32_t)(uintptr_t)gdt };

    /* Load the new table, then reload every segment register so the CPU
       re-reads the descriptors. CS can only be changed by a far jump. */
    __asm__ volatile (
        "lgdt %0\n\t"
        "ljmp %1, $1f\n"            /* far jump to the label "1:" just below */
        "1:\n\t"
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        : : "m"(ptr), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA) : "eax", "memory");
}
