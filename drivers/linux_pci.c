/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * linux_pci.c — Linux's PCI driver API on MyOS's PCI scan (pci.c).
 *
 * pci_register_driver walks the devices the boot scan found and calls the
 * driver's probe() for each one its id table matches (and no other driver
 * has). A BAR's size is found the first time a driver asks, the standard
 * way: write all ones, read back which bits stuck — with the device's
 * decoding off meanwhile, so it doesn't answer at a bogus address.
 */
#include <interrupts.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/slab.h>

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *d)
{
    for (; ids && (ids->vendor || ids->class_mask); ids++)
        if ((ids->vendor == PCI_ANY_ID || ids->vendor == d->vendor) &&
            (ids->device == PCI_ANY_ID || ids->device == d->device) &&
            (ids->subvendor == PCI_ANY_ID || ids->subvendor == d->subsystem_vendor) &&
            (ids->subdevice == PCI_ANY_ID || ids->subdevice == d->subsystem_device) &&
            !((ids->class ^ d->class) & ids->class_mask))
            return ids;
    return NULL;
}

/* The registered drivers, for a device that comes back (reprobe, rescan). */
#define DRIVERS_MAX 8
static struct pci_driver *drivers[DRIVERS_MAX];

static void bind(struct pci_dev *d, struct pci_driver *drv, const struct pci_device_id *id)
{
    d->driver = drv;
    d->dev.driver = &drv->driver;
    int err = drv->probe(d, id);
    if (err) {
        printk(KERN_INFO "%s: %s: probe failed (%d)\n", drv->name, pci_name(d), err);
        devres_release_all(&d->dev);
        d->driver = NULL;
        d->dev.driver = NULL;
        d->dev.driver_data = NULL;
    }
}

static void unbind(struct pci_dev *d)
{
    struct pci_driver *drv = d->driver;
    if (!drv)
        return;
    if (drv->remove)
        drv->remove(d);
    devres_release_all(&d->dev);
    d->driver = NULL;
    d->dev.driver = NULL;
    d->dev.driver_data = NULL;
}

/* The first registered driver that wants d. */
static void probe_any(struct pci_dev *d)
{
    for (int k = 0; k < DRIVERS_MAX && !d->driver; k++) {
        const struct pci_device_id *id = drivers[k] ? pci_match_id(drivers[k]->id_table, d) : NULL;
        if (id)
            bind(d, drivers[k], id);
    }
}

int pci_register_driver(struct pci_driver *drv)
{
    int k = 0;
    while (k < DRIVERS_MAX && drivers[k])
        k++;
    if (k == DRIVERS_MAX)
        return -ENOSPC;
    drivers[k] = drv;
    drv->driver.name = drv->name;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_dev_at(i);
        const struct pci_device_id *id = d->driver || d->removed ? NULL : pci_match_id(drv->id_table, d);
        if (id)
            bind(d, drv, id);
    }
    return 0;
}

void pci_unregister_driver(struct pci_driver *drv)
{
    for (int i = 0; i < pci_count(); i++)
        if (pci_dev_at(i)->driver == drv)
            unbind(pci_dev_at(i));
    for (int k = 0; k < DRIVERS_MAX; k++)
        if (drivers[k] == drv)
            drivers[k] = NULL;
}

/* --- a driver's last resorts: unbind, reprobe, remove, rescan, reset ------------ */

/* Only PCI devices have drivers here: the pci_dev behind dev, or NULL. */
static struct pci_dev *pci_of(struct device *dev)
{
    for (int i = 0; i < pci_count(); i++)
        if (&pci_dev_at(i)->dev == dev)
            return pci_dev_at(i);
    return NULL;
}

void device_release_driver(struct device *dev)
{
    struct pci_dev *d = pci_of(dev);
    if (d)
        unbind(d);
}

/* Off and on again: the driver starts from scratch (after its firmware
   kept failing). */
int device_reprobe(struct device *dev)
{
    struct pci_dev *d = pci_of(dev);
    if (!d)
        return -ENODEV;
    unbind(d);
    probe_any(d);
    return 0;
}

/* Linux forgets the device until the bus is scanned again; we keep it
   but hide it from drivers. */
void pci_stop_and_remove_bus_device(struct pci_dev *d)
{
    unbind(d);
    u16 cmd;
    pci_read_config_word(d, PCI_COMMAND, &cmd);     /* quiet: no DMA, no decoding */
    pci_write_config_word(d, PCI_COMMAND, cmd & ~(PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY | PCI_COMMAND_IO));
    d->removed = 1;
}

/* Is d on bus, or on a bus behind it? */
static bool below(const struct pci_bus *b, const struct pci_bus *bus)
{
    for (; b; b = b->parent)
        if (b == bus)
            return true;
    return false;
}

/* The removed devices under bus come back and meet the drivers again.
   (Devices plugged in since boot aren't found: we don't scan again.) */
unsigned int pci_rescan_bus(struct pci_bus *bus)
{
    unsigned int n = 0;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_dev_at(i);
        if (d->removed && below(d->bus, bus)) {
            d->removed = 0;
            probe_any(d);
            n++;
        }
    }
    return n;
}

/* Function Level Reset (PCIe): just this device goes back to its power-on
   state, firmware and all. Its configuration (BARs, command, interrupt
   setup) is saved before and written back after, as Linux does: the
   capabilities first, then the header from the end down, each dword only
   if the reset changed it. Sleeps about 100 ms (the spec's wait).
   -ENOTTY if the device can't do it. */
int pci_reset_function(struct pci_dev *d)
{
    int cap = pci_find_capability(d, PCI_CAP_ID_EXP);
    u32 devcap = 0, saved[64], now;
    if (cap)
        pci_read_config_dword(d, cap + PCI_EXP_DEVCAP, &devcap);
    if (!(devcap & PCI_EXP_DEVCAP_FLR))
        return -ENOTTY;
    for (int i = 0; i < 64; i++)
        pci_read_config_dword(d, i * 4, &saved[i]);
    for (int t = 0; t < 100; t++) {     /* let what it started finish */
        u16 sta;
        pci_read_config_word(d, cap + PCI_EXP_DEVSTA, &sta);
        if (!(sta & PCI_EXP_DEVSTA_TRPND))
            break;
        msleep(1);
    }
    u16 ctl;
    pci_read_config_word(d, cap + PCI_EXP_DEVCTL, &ctl);
    pci_write_config_word(d, cap + PCI_EXP_DEVCTL, ctl | PCI_EXP_DEVCTL_BCR_FLR);
    msleep(100);
    for (int t = 0; t < 1000; t++) {    /* still coming back: all ones */
        pci_read_config_dword(d, 0, &now);
        if (now != 0xffffffff)
            break;
        msleep(1);
    }
    for (int i = 63; i >= 1; i--) {     /* (64-16: capabilities; 15-1: the header) */
        pci_read_config_dword(d, i * 4, &now);
        if (now != saved[i])
            pci_write_config_dword(d, i * 4, saved[i]);
    }
    return 0;
}

/* --- configuration space ------------------------------------------------------ */

int pci_read_config_dword(const struct pci_dev *d, int where, u32 *val)
{
    *val = pci_read_config(d, (uint8_t)where);
    return 0;
}

int pci_read_config_word(const struct pci_dev *d, int where, u16 *val)
{
    *val = (u16)(pci_read_config(d, (uint8_t)(where & ~3)) >> ((where & 2) * 8));
    return 0;
}

int pci_read_config_byte(const struct pci_dev *d, int where, u8 *val)
{
    *val = (u8)(pci_read_config(d, (uint8_t)(where & ~3)) >> ((where & 3) * 8));
    return 0;
}

int pci_write_config_dword(const struct pci_dev *d, int where, u32 val)
{
    pci_write_config(d, (uint8_t)where, val);
    return 0;
}

/* Smaller writes: read the dword, change our part, write it back. (Port
   I/O config access is 32-bit here; the other bytes go back unchanged.) */
int pci_write_config_word(const struct pci_dev *d, int where, u16 val)
{
    int sh = (where & 2) * 8;
    u32 v = pci_read_config(d, (uint8_t)(where & ~3));
    pci_write_config(d, (uint8_t)(where & ~3), (v & ~(0xffffu << sh)) | (u32)val << sh);
    return 0;
}

int pci_write_config_byte(const struct pci_dev *d, int where, u8 val)
{
    int sh = (where & 3) * 8;
    u32 v = pci_read_config(d, (uint8_t)(where & ~3));
    pci_write_config(d, (uint8_t)(where & ~3), (v & ~(0xffu << sh)) | (u32)val << sh);
    return 0;
}

int pci_find_ext_capability(struct pci_dev *d, int cap)
{
    (void)d;
    (void)cap;
    return 0;                           /* beyond 256 bytes: not reachable yet */
}

int pcie_capability_read_word(struct pci_dev *d, int pos, u16 *val)
{
    int cap = pci_find_capability(d, PCI_CAP_ID_EXP);
    *val = 0;
    return cap ? pci_read_config_word(d, cap + pos, val) : -EINVAL;
}

int pcie_capability_read_dword(struct pci_dev *d, int pos, u32 *val)
{
    int cap = pci_find_capability(d, PCI_CAP_ID_EXP);
    *val = 0;
    return cap ? pci_read_config_dword(d, cap + pos, val) : -EINVAL;
}

int pcie_capability_write_word(struct pci_dev *d, int pos, u16 val)
{
    int cap = pci_find_capability(d, PCI_CAP_ID_EXP);
    return cap ? pci_write_config_word(d, cap + pos, val) : -EINVAL;
}

int pcie_capability_clear_and_set_word(struct pci_dev *d, int pos, u16 clear, u16 set)
{
    u16 v;
    int err = pcie_capability_read_word(d, pos, &v);
    return err ? err : pcie_capability_write_word(d, pos, (u16)((v & ~clear) | set));
}

/* --- enabling, BARs, mappings ------------------------------------------------- */

int pci_enable_device(struct pci_dev *d)
{
    u16 cmd;
    pci_read_config_word(d, PCI_COMMAND, &cmd);
    pci_write_config_word(d, PCI_COMMAND, cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    return 0;
}

static void disable_dev(void *p)
{
    pci_disable_device(p);
}

int pcim_enable_device(struct pci_dev *d)
{
    pci_enable_device(d);
    return devm_add_action(&d->dev, disable_dev, d);
}

void pci_disable_device(struct pci_dev *d)
{
    u16 cmd;
    pci_read_config_word(d, PCI_COMMAND, &cmd);
    pci_write_config_word(d, PCI_COMMAND, cmd & ~(PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY));
}

void pci_set_master(struct pci_dev *d)
{
    u16 cmd;
    pci_read_config_word(d, PCI_COMMAND, &cmd);
    pci_write_config_word(d, PCI_COMMAND, cmd | PCI_COMMAND_MASTER);
}

void pci_clear_master(struct pci_dev *d)
{
    u16 cmd;
    pci_read_config_word(d, PCI_COMMAND, &cmd);
    pci_write_config_word(d, PCI_COMMAND, cmd & ~PCI_COMMAND_MASTER);
}

resource_size_t pci_resource_start(struct pci_dev *d, int bar)
{
    if (bar < 0 || bar > 5)
        return 0;
    return d->bar[bar] & 1 ? d->bar[bar] & ~3u : pci_bar_phys(d, bar);
}

resource_size_t pci_resource_len(struct pci_dev *d, int bar)
{
    if (bar < 0 || bar > 5 || !d->bar[bar])
        return 0;
    if (d->bar_len[bar])
        return d->bar_len[bar];
    uint8_t off = (uint8_t)(0x10 + bar * 4);
    int is64 = !(d->bar[bar] & 1) && (d->bar[bar] & 6) == 4 && bar < 5;
    u16 cmd;
    pci_read_config_word(d, PCI_COMMAND, &cmd);
    unsigned long f = irq_save();
    pci_write_config_word(d, PCI_COMMAND, cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO));
    pci_write_config(d, off, 0xFFFFFFFFu);
    u64 mask = pci_read_config(d, off);
    pci_write_config(d, off, d->bar[bar]);
    if (is64) {
        pci_write_config(d, (uint8_t)(off + 4), 0xFFFFFFFFu);
        mask |= (u64)pci_read_config(d, (uint8_t)(off + 4)) << 32;
        pci_write_config(d, (uint8_t)(off + 4), d->bar[bar + 1]);
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }
    pci_write_config_word(d, PCI_COMMAND, cmd);
    irq_restore(f);
    mask &= d->bar[bar] & 1 ? ~3ull : ~0xFull;
    if (d->bar[bar] & 1)
        mask |= 0xFFFFFFFFFFFF0000ull;  /* I/O BARs are 16 bits wide */
    d->bar_len[bar] = (resource_size_t)(~mask + 1);
    return d->bar_len[bar];
}

void __iomem *pci_iomap(struct pci_dev *d, int bar, unsigned long maxlen)
{
    resource_size_t start = pci_resource_start(d, bar), len = pci_resource_len(d, bar);
    if (!start || !len || (d->bar[bar] & 1))
        return NULL;                    /* (port I/O BARs: not mapped) */
    if (maxlen && maxlen < len)
        len = maxlen;
    if (len > 64u << 20)
        len = 64u << 20;                /* the MMIO window is 64 MiB in all */
    return (void __iomem *)pci_map_mmio(start, (uint32_t)len);
}

/* pcim_iomap_table: the BARs mapped so far, by number (Linux's devres
   table). Kept per device in a small array we allocate on first use. */
struct iomap_table { void __iomem *map[6]; };

static struct iomap_table *table_of(struct pci_dev *d)
{
    static struct { struct pci_dev *d; struct iomap_table t; } tables[8];
    for (int k = 0; k < 8; k++)
        if (tables[k].d == d)
            return &tables[k].t;
    for (int k = 0; k < 8; k++)
        if (!tables[k].d) {
            tables[k].d = d;
            return &tables[k].t;
        }
    return NULL;
}

void __iomem *pcim_iomap(struct pci_dev *d, int bar, unsigned long maxlen)
{
    struct iomap_table *t = table_of(d);
    if (t && bar >= 0 && bar < 6 && t->map[bar])
        return t->map[bar];
    void __iomem *p = pci_iomap(d, bar, maxlen);
    if (t && p && bar >= 0 && bar < 6)
        t->map[bar] = p;
    return p;
}

void __iomem *const *pcim_iomap_table(struct pci_dev *d)
{
    struct iomap_table *t = table_of(d);
    return t ? t->map : NULL;
}

int pcim_iomap_regions(struct pci_dev *d, int mask, const char *name)
{
    (void)name;
    for (int bar = 0; bar < 6; bar++)
        if ((mask & (1 << bar)) && !pcim_iomap(d, bar, 0))
            return -ENOMEM;
    return 0;
}

/* --- MSI ------------------------------------------------------------------------ */

int pci_enable_msi(struct pci_dev *d)
{
    int n = pci_alloc_irq_vectors(d, 1, 1, PCI_IRQ_MSI);
    if (n < 0)
        return n;
    d->irq = (uint8_t)pci_irq_vector(d, 0);
    d->msi_enabled = 1;
    return 0;
}

int pci_enable_msix_range(struct pci_dev *d, struct msix_entry *e, int min, int max)
{
    int n = pci_alloc_irq_vectors(d, (unsigned)min, (unsigned)max, PCI_IRQ_MSIX);
    if (n < 0)
        return n;
    for (int k = 0; k < n; k++)
        e[k].vector = (u32)pci_irq_vector(d, (unsigned)e[k].entry);
    d->msix_enabled = 1;
    return n;
}

/* Turn MSI/MSI-X back off at the device. (The IRQ numbers are not given
   back: the pool is handed out once, docs/gaps.md.) */
void pci_free_irq_vectors(struct pci_dev *d)
{
    uint8_t cap;
    if (d->irq_type == PCI_IRQ_MSIX && (cap = pci_find_capability(d, PCI_CAP_ID_MSIX)))
        pci_write_config(d, cap, pci_read_config(d, cap) & ~(1u << 31));
    else if (d->irq_type == PCI_IRQ_MSI && (cap = pci_find_capability(d, PCI_CAP_ID_MSI)))
        pci_write_config(d, cap, pci_read_config(d, cap) & ~(1u << 16));
    d->irq_type = 0;
    d->nr_irqs = 0;
    d->msi_enabled = d->msix_enabled = 0;
}

void pci_disable_msi(struct pci_dev *d) { pci_free_irq_vectors(d); }
void pci_disable_msix(struct pci_dev *d) { pci_free_irq_vectors(d); }
