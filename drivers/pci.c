/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pci.c — read the PCI configuration space (step 10)
 *
 * Every PCI device answers at two I/O ports. Write the address of the
 * register you want to 0xCF8, then read the 32-bit value from 0xCFC.
 * The address has a fixed shape:
 *
 *   bit 31      1, or the device ignores the access
 *   bits 23-16  bus
 *   bits 15-11  device (slot)
 *   bits 10-8   function (one card can be several functions)
 *   bits 7-2    register number (a 32-bit register)
 *
 * Empty slots read as 0xFFFF. We start at bus 0 and follow every
 * PCI-to-PCI bridge to the bus behind it. A laptop's NVMe SSD, its
 * Ethernet chip and its graphics card each sit behind a PCI Express root
 * port, on bus 1, 2, 3... (the MSI showed only bus 0 and no SSD,
 * 2026-10-09). The firmware has numbered those buses and given their
 * devices addresses already: a bridge's register 0x18 says which bus is
 * behind it (secondary) and the highest one below it (subordinate).
 */
#include <apic.h>
#include <errno.h>
#include <interrupts.h>
#include <io.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pci.h>
#include <linux/kernel.h>
#include <string.h>

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
#define PCI_MAX  64

static struct pci_dev devs[PCI_MAX];
static int ndevs;
/* Linux's struct pci_bus for each bus number: which bridge leads to it. */
static struct pci_bus buses[256];

static uint32_t cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xfc);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

uint32_t pci_read_config(const struct pci_dev *dev, uint8_t offset)
{
    return cfg_read(dev->busnr, dev->slot, dev->func, offset);
}

void pci_write_config(const struct pci_dev *dev, uint8_t offset, uint32_t value)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)dev->busnr << 16)
                  | ((uint32_t)dev->slot << 11)
                  | ((uint32_t)dev->func << 8)
                  | (offset & 0xfc);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, value);
}

static void add(uint8_t bus, uint8_t slot, uint8_t func, uint32_t id)
{
    if (ndevs == PCI_MAX)
        return;
    struct pci_dev *d = &devs[ndevs++];
    memset(d, 0, sizeof *d);
    d->busnr = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = (uint16_t)id;
    d->device = (uint16_t)(id >> 16);
    d->class_rev = cfg_read(bus, slot, func, 0x08);
    /* Linux's names for the same facts (linux/pci.h). */
    uint32_t sub = cfg_read(bus, slot, func, 0x2c);
    buses[bus].number = bus;
    d->bus = &buses[bus];
    d->devfn = (unsigned)slot << 3 | func;
    d->subsystem_vendor = (uint16_t)sub;
    d->subsystem_device = (uint16_t)(sub >> 16);
    d->revision = (uint8_t)d->class_rev;
    d->class = d->class_rev >> 8;
    snprintf(d->dev.name, sizeof d->dev.name, "0000:%02x:%02x.%x", bus, slot, func);
    d->dev.dma_mask = &d->dev.own_dma_mask;
    d->dev.own_dma_mask = d->dev.coherent_dma_mask = 0xFFFFFFFFull;  /* Linux's default */
    d->irq = (uint8_t)cfg_read(bus, slot, func, 0x3c);
    for (int i = 0; i < 6; i++)
        d->bar[i] = cfg_read(bus, slot, func, (uint8_t)(0x10 + i * 4));
    klog("pci: %02x:%02x.%x %04x:%04x class %06lx irq %u\n",
            bus, slot, func, d->vendor, d->device, d->class_rev >> 8, d->irq);
}

#define PCI_COMMAND_IO     0x1
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4

static uint8_t scanned[256 / 8];        /* buses walked (a loop can't recurse forever) */

static void scan_bus(uint8_t bus, int depth);

/* A PCI-to-PCI bridge (header type 1): walk the bus behind it. It must
   forward the memory and DMA of the devices there, so it gets memory,
   I/O and bus-master on — what Linux does when a device below it is
   enabled. */
static void bridge(uint8_t bus, uint8_t slot, uint8_t func, int dev, int depth)
{
    struct pci_dev *self = dev >= 0 ? &devs[dev] : NULL;   /* (the table may be full) */
    uint32_t nums = cfg_read(bus, slot, func, 0x18);
    uint8_t secondary = (uint8_t)(nums >> 8), subordinate = (uint8_t)(nums >> 16);
    if (secondary <= bus || subordinate < secondary) {
        kprintf("pci: bridge %02x:%02x.%x: no bus behind it (the firmware didn't number it)\n",
                bus, slot, func);
        return;
    }
    klog("pci: bridge %02x:%02x.%x -> bus %02x-%02x\n", bus, slot, func, secondary, subordinate);
    if (self) {
        uint32_t cmd = pci_read_config(self, 0x04);
        pci_write_config(self, 0x04, (cmd & 0xFFFF) | PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                                     PCI_COMMAND_MASTER);
        buses[secondary].self = self;
    }
    buses[secondary].number = secondary;
    buses[secondary].parent = &buses[bus];
    scan_bus(secondary, depth + 1);
}

/* Every device on the bus first, then the buses behind its bridges, so
   bus 0 keeps the order it always had (the first xHCI, the first HD Audio
   chip) and what is behind a bridge comes after it. */
static void scan_bus(uint8_t bus, int depth)
{
    if (depth > 8 || (scanned[bus / 8] & (1u << (bus % 8))))
        return;
    scanned[bus / 8] |= (uint8_t)(1u << (bus % 8));
    struct { uint8_t slot, func; int dev; } br[16];
    int nbr = 0;
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = cfg_read(bus, slot, 0, 0);
        if ((uint16_t)id == 0xFFFF)
            continue;
        uint8_t header = (uint8_t)(cfg_read(bus, slot, 0, 0x0c) >> 16);
        for (uint8_t func = 0; func < ((header & 0x80) ? 8 : 1); func++) {
            if (func) {
                id = cfg_read(bus, slot, func, 0);
                if ((uint16_t)id == 0xFFFF)
                    continue;
            }
            int before = ndevs;
            add(bus, slot, func, id);
            uint8_t type = (uint8_t)(cfg_read(bus, slot, func, 0x0c) >> 16) & 0x7F;
            if (type == 1 && nbr < 16) {
                br[nbr].slot = slot;
                br[nbr].func = func;
                br[nbr++].dev = ndevs > before ? before : -1;
            }
        }
    }
    for (int k = 0; k < nbr; k++)
        bridge(bus, br[k].slot, br[k].func, br[k].dev, depth);
}

void pci_init(void)
{
    buses[0].number = 0;
    scan_bus(0, 0);
}

struct pci_dev *pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device)
            return &devs[i];
    return NULL;
}

int pci_count(void)
{
    return ndevs;
}

struct pci_dev *pci_dev_at(int index)
{
    return index >= 0 && index < ndevs ? &devs[index] : NULL;
}

/* Device registers live in a window of kernel addresses after the e1000's
   (MMIO_BASE, 128 KiB; memlayout.h). Mappings are never undone. */
#define MMIO_WINDOW     (MMIO_BASE + 0x100000)
#define MMIO_WINDOW_END MMIO_END

static volatile void *map_window(phys_addr_t phys, uint32_t bytes, uint32_t flags)
{
    static uintptr_t next = MMIO_WINDOW;
    uint32_t off = (uint32_t)phys & (PAGE_SIZE - 1);
    uint32_t pages = (off + bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!bytes || pages > (MMIO_WINDOW_END - next) / PAGE_SIZE)
        return NULL;
#ifndef ARCH_X86_64
    if (phys + (uint64_t)pages * PAGE_SIZE > 0x100000000ull)
        return NULL;                    /* a 4 KiB i386 page can't reach it */
#endif
    uint32_t f = irq_save();            /* (the ACPI thread maps too) */
    uintptr_t va = next;
    next += pages * PAGE_SIZE;
    irq_restore(f);
    for (uint32_t i = 0; i < pages; i++)
        if (vmm_map(va + i * PAGE_SIZE, (phys - off) + (phys_addr_t)i * PAGE_SIZE, flags) < 0)
            return NULL;
    return (volatile void *)(va + off);
}

volatile void *pci_map_mmio(phys_addr_t phys, uint32_t bytes)
{
    return map_window(phys, bytes, PTE_WRITABLE | PTE_PCD);
}

/* RAM the firmware keeps for itself (ACPI tables) above the direct map:
   cached, like the rest of RAM. The AML interpreter reads the tables a
   byte at a time; through an uncached mapping each byte would be a trip
   to memory. Never device registers. */
volatile void *ioremap_cache(phys_addr_t phys, uint32_t bytes)
{
    return map_window(phys, bytes, PTE_WRITABLE);
}

/* Config space by address, 1, 2 or 4 bytes at an offset aligned to its
   size (the ACPI interpreter's PCI accesses: AML names a device by bus,
   slot and function). Port 0xCFC takes a byte or a word at 0xCFC +
   (offset & 3), so a narrow write changes only its own bytes, not the
   status bits next to it. The first 256 bytes only (no MCFG yet). */
uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, int width)
{
    if (offset >= 256)
        return 0xFFFFFFFFu;
    uint32_t f = irq_save(), v;         /* the address and data ports are one pair */
    outl(PCI_ADDR, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)slot << 11 |
                   (uint32_t)func << 8 | (offset & 0xFC));
    uint16_t port = (uint16_t)(PCI_DATA + (offset & 3));
    v = width == 1 ? inb(port) : width == 2 ? inw(port) : inl(PCI_DATA);
    irq_restore(f);
    return v;
}

void pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, int width,
                   uint32_t value)
{
    if (offset >= 256)
        return;
    uint32_t f = irq_save();
    outl(PCI_ADDR, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)slot << 11 |
                   (uint32_t)func << 8 | (offset & 0xFC));
    uint16_t port = (uint16_t)(PCI_DATA + (offset & 3));
    if (width == 1)
        outb(port, (uint8_t)value);
    else if (width == 2)
        outw(port, (uint16_t)value);
    else
        outl(PCI_DATA, value);
    irq_restore(f);
}

phys_addr_t pci_bar_phys(const struct pci_dev *dev, int n)
{
    uint32_t b = dev->bar[n];
    if (b & 1)
        return 0;                       /* I/O ports, not memory */
    phys_addr_t pa = b & ~0xFu;
    if ((b & 6) == 4 && n < 5)          /* 64-bit: the next BAR is the high half */
        pa |= (phys_addr_t)dev->bar[n + 1] << 32;
    return pa;
}

/* --- capabilities and MSI ------------------------------------------------------ */

/* Capabilities are a linked list in config space: the status register says
   whether there is one, 0x34 points at the first, and each entry is
   { id, next, ... }. 48 steps at most, so a looped list can't hang us. */
uint8_t pci_find_capability(const struct pci_dev *dev, uint8_t id)
{
    if (!(pci_read_config(dev, 0x04) & (0x10u << 16)))
        return 0;
    uint8_t pos = pci_read_config(dev, 0x34) & 0xFC;
    for (int n = 0; pos && n < 48; n++) {
        uint32_t v = pci_read_config(dev, pos);
        if ((v & 0xFF) == id)
            return pos;
        pos = (v >> 8) & 0xFC;
    }
    return 0;
}

/* Bits of the PCI command register (offset 4) */
#define CMD_MEMORY     (1u << 1)
#define CMD_INTX_OFF   (1u << 10)

/* The capability's first word is { id, next, control }; only the control
   half (the top 16 bits) is ours to write. */
static void set_control(const struct pci_dev *dev, uint8_t cap, uint16_t ctrl)
{
    pci_write_config(dev, cap, (pci_read_config(dev, cap) & 0xFFFF) | (uint32_t)ctrl << 16);
}

/* MSI-X: a table of {address, data, mask} entries in one of the device's
   BARs, one entry per interrupt it can raise. Fill the first n (min to
   max, as many as the table has) with our messages, then switch MSI-X on.
   n, or -1 with nothing changed. */
static int msix_setup(struct pci_dev *dev, uint8_t cap, int min, int n)
{
    uint16_t ctrl = (uint16_t)(pci_read_config(dev, cap) >> 16);
    int size = (ctrl & 0x7FF) + 1;
    if (n > size)
        n = size;
    if (n < min)
        return -1;
    uint32_t where = pci_read_config(dev, cap + 4);   /* BAR number + offset */
    phys_addr_t bar = pci_bar_phys(dev, where & 7);
    volatile uint32_t *t = bar ? pci_map_mmio(bar + (where & ~7u), (uint32_t)size * 16) : NULL;
    int first = t ? msi_irq_alloc(n) : -1;
    if (first < 0)
        return -1;
    pci_write_config(dev, 4, pci_read_config(dev, 4) | CMD_MEMORY | CMD_INTX_OFF);
    for (int k = 0; k < n; k++) {
        uint32_t lo, hi, data;
        msi_compose(first + k, &lo, &hi, &data);
        t[k * 4 + 0] = lo;
        t[k * 4 + 1] = hi;
        t[k * 4 + 2] = data;
        t[k * 4 + 3] = 0;               /* unmasked */
    }
    set_control(dev, cap, (ctrl & ~(1u << 14)) | (1u << 15));    /* on, not all masked */
    dev->irq_type = PCI_IRQ_MSIX;
    dev->irq_base = first;
    dev->nr_irqs = (uint8_t)n;
    return n;
}

/* MSI: one address and one data word in config space. One IRQ. */
static int msi_setup(struct pci_dev *dev, uint8_t cap)
{
    uint16_t ctrl = (uint16_t)(pci_read_config(dev, cap) >> 16);
    int irq = msi_irq_alloc(1);
    if (irq < 0)
        return -1;
    uint32_t lo, hi, data;
    msi_compose(irq, &lo, &hi, &data);
    int is64 = (ctrl & 0x80) != 0;
    pci_write_config(dev, cap + 4, lo);
    if (is64)
        pci_write_config(dev, cap + 8, hi);
    pci_write_config(dev, cap + (is64 ? 0xC : 8), data);
    if (ctrl & 0x100)                   /* per-vector masking: unmask ours */
        pci_write_config(dev, cap + (is64 ? 0x10 : 0xC), 0);
    pci_write_config(dev, 4, pci_read_config(dev, 4) | CMD_INTX_OFF);
    set_control(dev, cap, (ctrl & ~0x70u) | 1);     /* on, one message */
    dev->irq_type = PCI_IRQ_MSI;
    dev->irq_base = irq;
    dev->nr_irqs = 1;
    return 1;
}

int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min, unsigned int max,
                          unsigned int flags)
{
    uint8_t cap;
    if (min < 1 || max < min)
        return -EINVAL;
    if ((flags & (PCI_IRQ_MSIX | PCI_IRQ_MSI)) && lapic_enable() == 0) {
        if ((flags & PCI_IRQ_MSIX) && (cap = pci_find_capability(dev, PCI_CAP_ID_MSIX)) &&
            msix_setup(dev, cap, (int)min, (int)max) > 0)
            return dev->nr_irqs;
        if ((flags & PCI_IRQ_MSI) && min == 1 &&
            (cap = pci_find_capability(dev, PCI_CAP_ID_MSI)) && msi_setup(dev, cap) == 1)
            return 1;
    }
    if ((flags & PCI_IRQ_INTX) && min == 1 && dev->irq && dev->irq < 16) {
        dev->irq_type = PCI_IRQ_INTX;
        dev->irq_base = dev->irq;
        dev->nr_irqs = 1;
        return 1;
    }
    return -ENOSPC;
}

int pci_irq_vector(const struct pci_dev *dev, unsigned int nr)
{
    return nr < dev->nr_irqs ? dev->irq_base + (int)nr : -EINVAL;
}
