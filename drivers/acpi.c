/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * acpi.c — what "power off" needs from the firmware's ACPI tables
 *
 * A PC switches itself off when the OS writes a number to its power-
 * management control port: "go to sleep state S5" (soft off). Which port
 * and which number differ per board, and the firmware says them in its
 * ACPI tables:
 *
 *   RSDP  "RSD PTR ": where the table of tables is. UEFI hands it to the
 *         loader (bootinfo); on a BIOS PC it sits in the BIOS area.
 *   RSDT / XSDT  the table of tables (32- or 64-bit addresses).
 *   FADT  ("FACP") the fixed hardware: the PM1a/PM1b control ports, and
 *         where the DSDT is.
 *   DSDT  the board's description in AML bytecode. Its object _S5 is a
 *         small package whose first two numbers are the sleep types for
 *         PM1a and PM1b.
 *
 * Reading AML properly needs an interpreter (Linux has ACPICA, 100,000
 * lines). For _S5 we don't: it is a constant package on nearly every board
 * ("Name (_S5, Package () { 5, 5, 0, 0 })"), so we find its bytes and read
 * the two numbers, as small kernels commonly do. Everything is looked up
 * once, at boot (on i386, mappings must exist before the first process);
 * power off then only writes two ports. A board where this finds nothing
 * keeps the old behaviour: QEMU's own switch, or halt with a message.
 *
 * Not yet: "hardware-reduced" ACPI (some tablets: no PM1 ports, a sleep
 * register instead), _PTS (the "prepare to sleep" method some boards want
 * run first), and any other sleep state.
 */
#include <acpi.h>
#include <bootinfo.h>
#include <io.h>
#include <kprintf.h>
#include <linux/io.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>

#define SLP_EN       (1u << 13)         /* PM1 control: "sleep now" */
#define SLP_TYP_MASK (7u << 10)         /* ... in this sleep type */
#define SCI_EN       (1u << 0)          /* ACPI mode is on */
#define MAX_TABLE    (4u << 20)         /* no real table is bigger */

struct sdt {                            /* every table's header, 36 bytes */
    char sig[4];
    uint32_t len;
    uint8_t rev, csum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

static uint16_t pm1a_cnt, pm1b_cnt;     /* I/O ports; 0 = none */
static uint16_t slp_typ_a, slp_typ_b;
static uint32_t smi_cmd;
static uint8_t acpi_enable;
static int ready;

/* Physical memory to read: the direct map when it covers it (cached, and
   no window space used), else an uncached mapping of its own. Tables sit
   in firmware-reserved RAM, below 4 GiB on every PC we know. */
static const void *map(uint64_t phys, uint32_t len)
{
    if (!phys || len > MAX_TABLE || phys > 0xFFFFFFFFull - len)
        return NULL;
    if (phys + len <= pmm_end())
        return P2V(phys);
    return (const void *)ioremap((uint32_t)phys, len);
}

static int sum_ok(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint8_t s = 0;
    while (n--)
        s += *b++;
    return s == 0;
}

/* The table at phys, if it has this signature, a sane length and a good
   checksum. */
static const struct sdt *table(uint64_t phys, const char *sig)
{
    const struct sdt *h = map(phys, sizeof *h);
    if (!h || memcmp(h->sig, sig, 4) || h->len < sizeof *h || h->len > MAX_TABLE)
        return NULL;
    const struct sdt *t = map(phys, h->len);
    return t && sum_ok(t, t->len) ? t : NULL;
}

/* "RSD PTR " on a 16-byte boundary, with a good checksum (20 bytes). */
static uint64_t scan(uint32_t base, uint32_t len)
{
    for (uint32_t off = 0; off + 20 <= len; off += 16) {
        const uint8_t *p = P2V(base + off);
        if (!memcmp(p, "RSD PTR ", 8) && sum_ok(p, 20))
            return base + off;
    }
    return 0;
}

static uint64_t find_rsdp(void)
{
#ifdef ARCH_X86_64
    const struct bootinfo *bi = P2V(BOOTINFO_PHYS);
    if (bi->acpi_rsdp)
        return bi->acpi_rsdp;           /* UEFI told the loader */
#endif
    /* A BIOS PC: the first KiB of the EBDA (its segment is at 0x40E), then
       the BIOS ROM area. Both are in the first MiB, which is mapped. */
    uint32_t ebda = (uint32_t)*(const uint16_t *)P2V(0x40E) << 4;
    uint64_t r = ebda >= 0x80000 && ebda < 0xA0000 ? scan(ebda, 1024) : 0;
    return r ? r : scan(0xE0000, 0x20000);
}

/* One element of the _S5 package: Zero, One, or a BytePrefix/WordPrefix
   constant. -1 for anything else (an expression: give up). */
static int aml_const(const uint8_t *a, uint32_t len, uint32_t *p)
{
    if (*p >= len)
        return -1;
    switch (a[*p]) {
    case 0x00: (*p)++; return 0;                        /* ZeroOp */
    case 0x01: (*p)++; return 1;                        /* OneOp */
    case 0x0A:                                          /* BytePrefix */
        if (*p + 1 >= len)
            return -1;
        *p += 2;
        return a[*p - 1];
    case 0x0B:                                          /* WordPrefix */
        if (*p + 2 >= len)
            return -1;
        *p += 3;
        return a[*p - 2] | a[*p - 1] << 8;
    default:
        return -1;
    }
}

/* Name (_S5, Package (n) { A, B, ... }): NameOp, "_S5_", PackageOp,
   PkgLength (1-4 bytes: the first byte's top two bits say how many follow),
   NumElements, then the elements. */
static int find_s5(const uint8_t *a, uint32_t len)
{
    for (uint32_t i = 2; i + 4 < len; i++) {
        if (memcmp(a + i, "_S5_", 4))
            continue;
        if (a[i - 1] != 0x08 && !(a[i - 1] == '\\' && a[i - 2] == 0x08))
            continue;                   /* a use of _S5, not its definition */
        uint32_t p = i + 4;
        if (p + 3 >= len || a[p] != 0x12)
            continue;                   /* not a package: a method, maybe */
        p++;
        p += 1 + (a[p] >> 6);           /* PkgLength */
        p++;                            /* NumElements */
        int x = aml_const(a, len, &p), y = aml_const(a, len, &p);
        if (x < 0 || y < 0)
            continue;
        slp_typ_a = (uint16_t)(x & 7);
        slp_typ_b = (uint16_t)(y & 7);
        return 1;
    }
    return 0;
}

/* An I/O port from a 32-bit FADT field, or its 64-bit "generic address"
   twin (space 1 = I/O) when the old one is 0. */
static uint16_t port(const uint8_t *f, uint32_t flen, uint32_t old_at, uint32_t gas_at)
{
    uint32_t v;
    memcpy(&v, f + old_at, 4);
    if (v && v <= 0xFFFF)
        return (uint16_t)v;
    if (flen >= gas_at + 12 && f[gas_at] == 1) {
        uint64_t a;
        memcpy(&a, f + gas_at + 4, 8);
        if (a && a <= 0xFFFF)
            return (uint16_t)a;
    }
    return 0;
}

void acpi_init(void)
{
    uint64_t rp = find_rsdp();
    const uint8_t *r = rp ? map(rp, 36) : NULL;
    if (!r) {
        klog("acpi: no RSDP: power off is a halt\n");
        return;
    }
    /* The XSDT (64-bit addresses) on ACPI 2.0 and later, else the RSDT. */
    uint32_t rsdt;
    uint64_t xsdt = 0;
    memcpy(&rsdt, r + 16, 4);
    if (r[15] >= 2 && sum_ok(r, 36))
        memcpy(&xsdt, r + 24, 8);
    const struct sdt *root = xsdt ? table(xsdt, "XSDT") : NULL;
    int wide = root != NULL;
    if (!root)
        root = table(rsdt, "RSDT");
    if (!root) {
        klog("acpi: no RSDT/XSDT\n");
        return;
    }
    const uint8_t *fadt = NULL;
    uint32_t n = (root->len - sizeof *root) / (wide ? 8 : 4);
    for (uint32_t k = 0; k < n && !fadt; k++) {
        uint64_t at = 0;
        memcpy(&at, (const uint8_t *)root + sizeof *root + k * (wide ? 8 : 4), wide ? 8 : 4);
        fadt = (const uint8_t *)table(at, "FACP");
    }
    if (!fadt) {
        klog("acpi: no FADT\n");
        return;
    }
    uint32_t flen = ((const struct sdt *)fadt)->len, flags = 0, dsdt32 = 0;
    uint64_t dsdt = 0;
    if (flen >= 116)
        memcpy(&flags, fadt + 112, 4);
    if (flags & (1u << 20)) {
        klog("acpi: hardware-reduced ACPI: power off is not written for it yet\n");
        return;
    }
    pm1a_cnt = port(fadt, flen, 64, 172);
    pm1b_cnt = port(fadt, flen, 68, 184);
    memcpy(&smi_cmd, fadt + 48, 4);
    acpi_enable = fadt[52];
    memcpy(&dsdt32, fadt + 40, 4);
    if (flen >= 148)
        memcpy(&dsdt, fadt + 140, 8);
    const struct sdt *d = table(dsdt ? dsdt : dsdt32, "DSDT");
    if (!pm1a_cnt || !d || !find_s5((const uint8_t *)d + sizeof *d, d->len - sizeof *d)) {
        klog("acpi: %s: power off is a halt\n", !pm1a_cnt ? "no PM1a control port"
                                               : !d ? "no DSDT" : "no _S5 in the DSDT");
        return;
    }
    ready = 1;
    klog("acpi: power off = sleep type %u on port %x%s\n", slp_typ_a, pm1a_cnt,
         pm1b_cnt ? " (and PM1b)" : "");
}

int acpi_can_power_off(void)
{
    return ready;
}

int acpi_power_off(void)
{
    if (!ready)
        return -1;
    /* Firmware in "legacy mode" ignores the sleep bits until asked to hand
       the hardware to the OS (SMI_CMD <- ACPI_ENABLE). Bounded wait. */
    if (!(inw(pm1a_cnt) & SCI_EN) && smi_cmd && smi_cmd <= 0xFFFF && acpi_enable) {
        outb((uint16_t)smi_cmd, acpi_enable);
        for (int i = 0; i < 3000000 && !(inw(pm1a_cnt) & SCI_EN); i++)
            ;
    }
    /* As Linux does: the type first, then "sleep now" with it. */
    uint16_t a = (inw(pm1a_cnt) & ~(SLP_TYP_MASK | SLP_EN)) | (uint16_t)(slp_typ_a << 10);
    outw(pm1a_cnt, a);
    if (pm1b_cnt) {
        uint16_t b = (inw(pm1b_cnt) & ~(SLP_TYP_MASK | SLP_EN)) | (uint16_t)(slp_typ_b << 10);
        outw(pm1b_cnt, b);
        outw(pm1b_cnt, b | SLP_EN);
    }
    outw(pm1a_cnt, a | SLP_EN);
    for (volatile int i = 0; i < 10000000; i++)
        ;                               /* the board takes a moment to cut the power */
    return -1;                          /* still here: it didn't */
}
