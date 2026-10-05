/*
 * gdt.c — x86_64 GDT + 64-bit TSS (RSP0 for syscall/interrupt from ring 3)
 */
#include <gdt.h>
#include <stdint.h>
#include <string.h>

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

static struct gdt_entry gdt[7];
struct tss64 kernel_tss;

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
    set_entry(1, 0, 0, 0x9A, 0xA);   /* 64-bit code, L=1 in flags nibble */
    set_entry(2, 0, 0, 0x92, 0xA);   /* data */
    set_entry(3, 0, 0, 0xFA, 0xA);     /* user code RPL 3 */
    set_entry(4, 0, 0, 0xF2, 0xA);     /* user data RPL 3 */

    kernel_tss.iomap_base = sizeof kernel_tss;
    set_tss(5, (uint64_t)(uintptr_t)&kernel_tss, sizeof kernel_tss - 1);

    struct gdt_ptr ptr = { sizeof gdt - 1, (uint64_t)(uintptr_t)gdt };
    __asm__ volatile ("lgdt %0" : : "m"(ptr) : "memory");
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    kernel_tss.rsp0 = rsp0;
}

void gdt_load_tls(const uint64_t tls[GDT_TLS_ENTRIES])
{
    (void)tls;
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
