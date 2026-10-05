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
 * Empty slots read as 0xFFFF. We walk bus 0 only: that is the bus QEMU's
 * i440FX and a typical old PC put the NIC on. Bridges to other buses come
 * with the machines that have them.
 */
#include <io.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pci.h>
#include <string.h>

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
#define PCI_MAX  32

static struct pci_dev devs[PCI_MAX];
static int ndevs;

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
    return cfg_read(dev->bus, dev->slot, dev->func, offset);
}

void pci_write_config(const struct pci_dev *dev, uint8_t offset, uint32_t value)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)dev->bus << 16)
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
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = (uint16_t)id;
    d->device = (uint16_t)(id >> 16);
    d->class_rev = cfg_read(bus, slot, func, 0x08);
    d->irq = (uint8_t)cfg_read(bus, slot, func, 0x3c);
    for (int i = 0; i < 6; i++)
        d->bar[i] = cfg_read(bus, slot, func, (uint8_t)(0x10 + i * 4));
    kprintf("pci: %02x:%02x.%x %04x:%04x class %06lx irq %u\n",
            bus, slot, func, d->vendor, d->device, d->class_rev >> 8, d->irq);
}

void pci_init(void)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = cfg_read(0, slot, 0, 0);
        if ((uint16_t)id == 0xFFFF)
            continue;
        add(0, slot, 0, id);
        uint8_t header = (uint8_t)(cfg_read(0, slot, 0, 0x0c) >> 16);
        if (!(header & 0x80))
            continue;                   /* a single function */
        for (uint8_t func = 1; func < 8; func++) {
            id = cfg_read(0, slot, func, 0);
            if ((uint16_t)id != 0xFFFF)
                add(0, slot, func, id);
        }
    }
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
   (MMIO_BASE, 128 KiB; memlayout.h). Mappings are made once, at boot, and
   never undone. */
#define MMIO_WINDOW     (MMIO_BASE + 0x100000)
#define MMIO_WINDOW_END MMIO_END

volatile void *pci_map_mmio(uint32_t phys, uint32_t bytes)
{
    static uintptr_t next = MMIO_WINDOW;
    uint32_t off = phys & (PAGE_SIZE - 1);
    uint32_t pages = (off + bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!bytes || pages > (MMIO_WINDOW_END - next) / PAGE_SIZE)
        return NULL;
    uintptr_t va = next;
    for (uint32_t i = 0; i < pages; i++)
        if (vmm_map(va + i * PAGE_SIZE, (phys - off) + i * PAGE_SIZE,
                    PTE_WRITABLE | PTE_PCD) < 0)
            return NULL;
    next += pages * PAGE_SIZE;
    return (volatile void *)(va + off);
}
