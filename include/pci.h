#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <linux/device.h>
#include <types.h>

/*
 * A device on the PCI bus, found by reading configuration space (ports
 * 0xCF8 and 0xCFC). Enough for a NIC driver to see its vendor, device,
 * IRQ line and BARs, its capabilities, and MSI/MSI-X interrupts.
 */

#define PCI_ANY_ID (~0u)

struct pci_driver;
struct pci_bus {
    unsigned char number;
    struct pci_dev *self;           /* the bridge leading to it (NULL: bus 0) */
    struct pci_bus *parent;         /* the bus that bridge sits on (NULL: bus 0) */
};

struct pci_dev {
    uint8_t busnr, slot, func;
    uint16_t vendor, device;
    uint32_t class_rev;             /* class in the top 24 bits, rev in the low 8 */
    uint8_t irq;                    /* its 8259 line (255 or 0: none) */
    uint32_t bar[6];
    /* After pci_alloc_irq_vectors: nr_irqs IRQs from irq_base on. */
    uint8_t irq_type;               /* PCI_IRQ_INTX, PCI_IRQ_MSI or PCI_IRQ_MSIX */
    uint8_t nr_irqs;
    int irq_base;

    /* What a Linux driver reads (linux/pci.h): the same facts, Linux's names. */
    struct device dev;              /* name "0000:00:14.3", drvdata, devres */
    struct pci_bus *bus;
    unsigned int devfn;             /* slot << 3 | function */
    uint16_t subsystem_vendor, subsystem_device;
    uint8_t revision;
    unsigned int class;             /* class << 16 | subclass << 8 | prog-if */
    struct pci_driver *driver;      /* bound by pci_register_driver */
    phys_addr_t bar_len[6];         /* sized when a driver asks (0: not yet) */
    uint8_t msi_enabled, msix_enabled;
    uint8_t removed;                /* pci_stop_and_remove_bus_device: no driver gets it
                                       until pci_rescan_bus */
};

/* pci_alloc_irq_vectors flags, Linux's names and values */
#define PCI_IRQ_INTX  (1u << 0)
#define PCI_IRQ_MSI   (1u << 1)
#define PCI_IRQ_MSIX  (1u << 2)

#define PCI_CAP_ID_MSI   0x05
#define PCI_CAP_ID_MSIX  0x11

void pci_init(void);                /* scan bus 0 and print what is there */

/* The first device with this vendor and device id, or NULL. */
struct pci_dev *pci_find(uint16_t vendor, uint16_t device);

int pci_count(void);
struct pci_dev *pci_dev_at(int index);

/* Configuration space. offset is a byte offset; the access is 32-bit. */
uint32_t pci_read_config(const struct pci_dev *dev, uint8_t offset);
void pci_write_config(const struct pci_dev *dev, uint8_t offset, uint32_t value);

/* Map a device's registers (a memory BAR at `phys`, `bytes` long) into the
   kernel, uncached. The kernel address, or NULL if the window is full (or,
   on i386, `phys` is above 4 GiB). */
volatile void *pci_map_mmio(phys_addr_t phys, uint32_t bytes);

/* Where memory BAR n starts (both halves of a 64-bit BAR); 0 for an I/O
   BAR or none. */
phys_addr_t pci_bar_phys(const struct pci_dev *dev, int n);

/* The offset of capability `id` in config space, or 0 (Linux's name). */
uint8_t pci_find_capability(const struct pci_dev *dev, uint8_t id);

/* Linux's interrupt setup: between min and max IRQs, by the first kind in
   `flags` that works — MSI-X, then MSI (1 IRQ), then the 8259 line.
   How many, or -ENOSPC. pci_irq_vector(dev, k) is the k-th, for
   irq_install. */
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min, unsigned int max,
                          unsigned int flags);
int pci_irq_vector(const struct pci_dev *dev, unsigned int nr);

#endif
