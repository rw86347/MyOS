#ifndef NVME_H
#define NVME_H

/* Find an NVMe SSD on the PCI bus and register namespace 1 as "nvme0n1". */
void nvme_init(void);

#endif
