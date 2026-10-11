/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_PCI_H
#define LINUX_PCI_H

/*
 * Linux's PCI driver API on MyOS's PCI code (drivers/pci.c, linux_pci.c).
 * A driver lists the devices it handles (an id table) and registers; for
 * each device found that matches, its probe() runs. struct pci_dev is the
 * kernel's own, with Linux's fields added (include/pci.h).
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <pci.h>

#define PCI_VENDOR_ID_INTEL    0x8086
#define PCI_VENDOR_ID_REDHAT   0x1b36
#define PCI_DEVFN(slot, fn)    ((((slot) & 0x1f) << 3) | ((fn) & 0x07))
#define PCI_SLOT(devfn)        (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)        ((devfn) & 0x07)

#define PCI_DEVICE(v, d) \
    .vendor = (v), .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(v, d, sv, sd) \
    .vendor = (v), .device = (d), .subvendor = (sv), .subdevice = (sd)
#define PCI_VDEVICE(v, d) \
    .vendor = PCI_VENDOR_ID_##v, .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, \
    .class = 0, .class_mask = 0
#define PCI_DEVICE_CLASS(c, m) \
    .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, \
    .subdevice = PCI_ANY_ID, .class = (c), .class_mask = (m)

struct pci_driver {
    const char *name;
    const struct pci_device_id *id_table;
    int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *dev);
    void (*shutdown)(struct pci_dev *dev);
    struct device_driver driver;    /* .pm: suspend/resume (not called yet) */
};

int  pci_register_driver(struct pci_driver *drv);
void pci_unregister_driver(struct pci_driver *drv);
#define module_pci_driver(drv) \
    int __register_##drv(void) { return pci_register_driver(&(drv)); }
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev);

#define to_pci_dev(d)                container_of(d, struct pci_dev, dev)
#define pci_name(p)                  dev_name(&(p)->dev)
static inline void *pci_get_drvdata(struct pci_dev *p) { return dev_get_drvdata(&p->dev); }
static inline void pci_set_drvdata(struct pci_dev *p, void *d) { dev_set_drvdata(&p->dev, d); }
static inline int pci_domain_nr(struct pci_bus *bus) { (void)bus; return 0; }
#define pci_dev_get(p)               ({ (p); })
#define pci_dev_put(p)               do { (void)(p); } while (0)

/* Configuration space (256 bytes through ports 0xCF8/0xCFC; the extended
   4 KiB needs memory-mapped config, which we don't do yet). 0 or -errno. */
int pci_read_config_byte(const struct pci_dev *d, int where, u8 *val);
int pci_read_config_word(const struct pci_dev *d, int where, u16 *val);
int pci_read_config_dword(const struct pci_dev *d, int where, u32 *val);
int pci_write_config_byte(const struct pci_dev *d, int where, u8 val);
int pci_write_config_word(const struct pci_dev *d, int where, u16 val);
int pci_write_config_dword(const struct pci_dev *d, int where, u32 val);
int pci_find_ext_capability(struct pci_dev *d, int cap);          /* 0: none (no ECAM) */
int pcie_capability_read_word(struct pci_dev *d, int pos, u16 *val);
int pcie_capability_read_dword(struct pci_dev *d, int pos, u32 *val);
int pcie_capability_write_word(struct pci_dev *d, int pos, u16 val);
int pcie_capability_clear_and_set_word(struct pci_dev *d, int pos, u16 clear, u16 set);
#define pcie_capability_set_word(d, pos, set)   pcie_capability_clear_and_set_word(d, pos, 0, set)
#define pcie_capability_clear_word(d, pos, clr) pcie_capability_clear_and_set_word(d, pos, clr, 0)
static inline struct pci_dev *pcie_find_root_port(struct pci_dev *d) { (void)d; return NULL; }
static inline bool pci_is_pcie(struct pci_dev *d) { return pci_find_capability(d, 0x10) != 0; }

#define PCI_COMMAND            0x04
#define PCI_COMMAND_IO         0x1
#define PCI_COMMAND_MEMORY     0x2
#define PCI_COMMAND_MASTER     0x4
#define PCI_COMMAND_INTX_DISABLE 0x400
#define PCI_CAP_ID_EXP         0x10
#define PCI_EXP_DEVCAP         4
#define PCI_EXP_DEVCAP_FLR     0x10000000  /* function level reset supported */
#define PCI_EXP_DEVCTL         8
#define PCI_EXP_DEVCTL_BCR_FLR 0x8000      /* start one */
#define PCI_EXP_DEVSTA         10
#define PCI_EXP_DEVSTA_TRPND   0x0020      /* transactions still pending */
#define PCI_EXP_DEVCTL2        40
#define PCI_EXP_LNKCTL         16
#define PCI_EXP_LNKCTL_ASPM_L0S 0x0001
#define PCI_EXP_LNKCTL_ASPM_L1 0x0002
#define PCI_EXP_DEVCTL2_LTR_EN 0x0400
#define PCI_EXP_LNKSTA         18          /* link status: the speed it trained at */
#define PCI_EXP_LNKSTA_CLS     0x000f
#define PCI_EXP_LNKSTA_CLS_2_5GB  0x0001
#define PCI_EXP_LNKSTA_CLS_5_0GB  0x0002
#define PCI_EXP_LNKSTA_CLS_8_0GB  0x0003
#define PCI_EXP_LNKSTA_CLS_16_0GB 0x0004
#define PCI_BASE_ADDRESS_0     0x10
#define PCI_BASE_ADDRESS_MEM_TYPE_MASK 0x06
#define PCI_BASE_ADDRESS_MEM_TYPE_64   0x04
#define PCI_EXT_CAP_ID_ERR     0x01        /* advanced error reporting */
#define PCI_ERR_ROOT_COMMAND   0x2c
#define PCI_ERR_ROOT_ERR_SRC   0x34

int  pci_enable_device(struct pci_dev *d);
int  pcim_enable_device(struct pci_dev *d);
void pci_disable_device(struct pci_dev *d);
void pci_set_master(struct pci_dev *d);
void pci_clear_master(struct pci_dev *d);
static inline int pci_request_regions(struct pci_dev *d, const char *n) { (void)d; (void)n; return 0; }
static inline void pci_release_regions(struct pci_dev *d) { (void)d; }
static inline int pcim_request_all_regions(struct pci_dev *d, const char *n) { (void)d; (void)n; return 0; }
static inline int pci_assign_resource(struct pci_dev *d, int bar) { (void)d; (void)bar; return 0; }
resource_size_t pci_resource_start(struct pci_dev *d, int bar);
resource_size_t pci_resource_len(struct pci_dev *d, int bar);
#define pci_resource_end(d, bar)  (pci_resource_start(d, bar) + pci_resource_len(d, bar) - 1)
void __iomem *pci_iomap(struct pci_dev *d, int bar, unsigned long maxlen);
void __iomem *pcim_iomap(struct pci_dev *d, int bar, unsigned long maxlen);
static inline void pci_iounmap(struct pci_dev *d, void __iomem *p) { (void)d; (void)p; }
void __iomem *const *pcim_iomap_table(struct pci_dev *d);
int  pcim_iomap_regions(struct pci_dev *d, int mask, const char *name);

int  pci_enable_msi(struct pci_dev *d);
void pci_disable_msi(struct pci_dev *d);
struct msix_entry { u32 vector; u16 entry; };
int  pci_enable_msix_range(struct pci_dev *d, struct msix_entry *e, int min, int max);
void pci_disable_msix(struct pci_dev *d);
void pci_free_irq_vectors(struct pci_dev *d);
#define PCI_IRQ_LEGACY   PCI_IRQ_INTX
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)
#define PCI_IRQ_AFFINITY 0

static inline bool pci_pme_capable(struct pci_dev *d, int state) { (void)d; (void)state; return false; }
static inline int pci_save_state(struct pci_dev *d) { (void)d; return 0; }
static inline void pci_restore_state(struct pci_dev *d) { (void)d; }
static inline int pci_set_power_state(struct pci_dev *d, int s) { (void)d; (void)s; return 0; }
static inline int pci_wake_from_d3(struct pci_dev *d, bool e) { (void)d; (void)e; return 0; }
static inline void pci_disable_link_state(struct pci_dev *d, int s) { (void)d; (void)s; }
static inline struct pci_dev *pci_get_slot(struct pci_bus *b, unsigned int devfn) { (void)b; (void)devfn; return NULL; }

/* ASPM: the link's power-saving states (pci_disable_link_state). */
#define PCIE_LINK_STATE_L0S    0x01
#define PCIE_LINK_STATE_L1     0x02
#define PCIE_LINK_STATE_CLKPM  0x04

/* The next device after `from` (NULL: the first) with these ids. */
static inline struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
                                             struct pci_dev *from)
{
    int i = 0;
    if (from) {
        while (i < pci_count() && pci_dev_at(i) != from)
            i++;
        i++;                        /* start after it */
    }
    for (; i < pci_count(); i++) {
        struct pci_dev *d = pci_dev_at(i);
        if ((vendor == PCI_ANY_ID || d->vendor == vendor) &&
            (device == PCI_ANY_ID || d->device == device))
            return d;
    }
    return NULL;
}
#define for_each_pci_dev(d) \
    for ((d) = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, NULL); (d); (d) = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, (d)))

#define pci_err(d, fmt, ...)   dev_err(&(d)->dev, fmt, ##__VA_ARGS__)
#define pci_warn(d, fmt, ...)  dev_warn(&(d)->dev, fmt, ##__VA_ARGS__)
#define pci_info(d, fmt, ...)  dev_info(&(d)->dev, fmt, ##__VA_ARGS__)
#define pci_dbg(d, fmt, ...)   dev_dbg(&(d)->dev, fmt, ##__VA_ARGS__)

/* Recovering a device that stopped answering: reset just this function
   (FLR), or take it off the bus and scan for it again. The PCI list is
   fixed at boot here, so rescanning finds nothing new (docs/gaps.md). */
int pci_reset_function(struct pci_dev *d);
#define pci_lock_rescan_remove()    do { } while (0)
#define pci_unlock_rescan_remove()  do { } while (0)
void pci_stop_and_remove_bus_device(struct pci_dev *d);
unsigned int pci_rescan_bus(struct pci_bus *bus);
#define PCI_D0       0
#define PCI_D3hot    3
#define PCI_D3cold   4

#endif
