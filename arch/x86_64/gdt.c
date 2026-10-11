/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * gdt.c — x86_64 GDT + 64-bit TSS (RSP0 for syscall/interrupt from ring 3)
 *
 * In long mode segments do almost nothing (base 0, no limit), but the CPU
 * still needs code/data descriptors for the privilege level, and a TSS for
 * the kernel stack to use when user code is interrupted.
 *
 * The order is fixed by syscall/sysret, which do not read the GDT: they
 * compute the selectors from MSR STAR (syscall.c). For sysret back to
 * 64-bit code the CPU takes SS = base + 8 and CS = base + 16. So user data
 * must come right before user code, with a spare slot before both:
 *
 *   0x00 null   0x08 kernel code   0x10 kernel data
 *   0x18 (sysret's base: a 32-bit user code segment, which we never use)
 *   0x20 user data (0x23 with RPL 3)   0x28 user code (0x2B)
 *   0x30 TSS (16 bytes: two slots)
 *
 * Linux has the same shape (its user code is 0x33).
 */
#include <cpu.h>
#include <gdt.h>
#include <stdint.h>
#include <string.h>

#define MSR_FS_BASE 0xC0000100

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[8];
struct tss64 kernel_tss;

/* A stack for the double fault alone (IST1, interrupts.c). A kernel stack
   that runs off its end hits its guard page (thread.c); the CPU can't push
   that page fault's frame onto the full stack, which makes it a double
   fault — and taken there too, a triple fault: the machine resets with
   nothing said. IST1 makes the CPU switch to this stack first. */
static uint8_t df_stack[8192] __attribute__((aligned(16)));

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].access      = access;
    gdt[i].flags_limit = (flags << 4) | ((limit >> 16) & 0x0F);
    gdt[i].base_high   = (base >> 24) & 0xFF;
}

static void set_tss(int i, uint64_t base, uint32_t limit)
{
    /* 64-bit TSS: two consecutive 8-byte slots (128-bit system descriptor). */
    struct {
        uint16_t limit0;
        uint16_t base0;
        uint8_t  base1;
        uint8_t  type;
        uint8_t  limit1;
        uint8_t  base2;
        uint32_t base3;
        uint32_t zero;
    } __attribute__((packed)) *d = (void *)&gdt[i];
    d->limit0 = limit & 0xFFFF;
    d->base0  = base & 0xFFFF;
    d->base1  = (base >> 16) & 0xFF;
    d->type   = 0x89;
    d->limit1 = (limit >> 16) & 0x0F;
    d->base2  = (base >> 24) & 0xFF;
    d->base3  = (uint32_t)(base >> 32);
    d->zero   = 0;
}

void gdt_init(void)
{
    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0, 0x9A, 0xA);          /* kernel code: 64-bit (L flag) */
    set_entry(2, 0, 0, 0x92, 0xA);          /* kernel data */
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xC);    /* user code, 32-bit: never loaded */
    set_entry(4, 0, 0, 0xF2, 0xA);          /* user data, DPL 3 */
    set_entry(5, 0, 0, 0xFA, 0xA);          /* user code, 64-bit, DPL 3 */

    kernel_tss.iomap_base = sizeof kernel_tss;  /* no I/O permission map */
    kernel_tss.ist[0] = (uint64_t)(uintptr_t)(df_stack + sizeof df_stack);
    set_tss(6, (uint64_t)(uintptr_t)&kernel_tss, sizeof kernel_tss - 1);

    struct gdt_ptr ptr = { sizeof gdt - 1, (uint64_t)(uintptr_t)gdt };
    __asm__ volatile ("lgdt %0" : : "m"(ptr) : "memory");
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    kernel_tss.rsp0 = rsp0;
}

/* A thread's thread-local storage: tls[0] is its FS base (arch_prctl).
   The value was checked to be a user address when it was set: writing a
   non-canonical one would fault in the kernel. */
void gdt_load_tls(const uint64_t tls[GDT_TLS_ENTRIES])
{
    wrmsr(MSR_FS_BASE, tls[0]);
}

uint64_t gdt_tls_descriptor(uint32_t base, uint32_t limit, uint32_t flags)
{
    (void)base;
    (void)limit;
    (void)flags;
    return 0;
}

int gdt_user_seg_ok(uint32_t sel)
{
    uint32_t idx = sel >> 3;
    if ((sel & 7) != 3 || idx >= sizeof gdt / sizeof gdt[0])
        return 0;
    uint8_t a = gdt[idx].access;
    return (a & 0xF0) == 0xF0;
}
