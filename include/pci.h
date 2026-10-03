#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/*
 * A device on the PCI bus, found by reading configuration space (ports
 * 0xCF8 and 0xCFC). Enough for a NIC driver to see its vendor, device,
 * IRQ line and BARs. Bus-master DMA and MSI come with the e1000 driver.
 */

#define PCI_ANY_ID 0xffff

struct pci_dev {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint32_t class_rev;             /* class in the top 24 bits, rev in the low 8 */
    uint8_t irq;
    uint32_t bar[6];
};

void pci_init(void);                /* scan bus 0 and print what is there */

/* The first device with this vendor and device id, or NULL. */
struct pci_dev *pci_find(uint16_t vendor, uint16_t device);

int pci_count(void);
struct pci_dev *pci_dev_at(int index);

/* Configuration space. offset is a byte offset; the access is 32-bit. */
uint32_t pci_read_config(const struct pci_dev *dev, uint8_t offset);
void pci_write_config(const struct pci_dev *dev, uint8_t offset, uint32_t value);

/* Map a device's registers (a memory BAR at `phys`, `bytes` long) into the
   kernel, uncached. The kernel address, or NULL if the window is full. */
volatile void *pci_map_mmio(uint32_t phys, uint32_t bytes);

#endif
