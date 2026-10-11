/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef NVME_H
#define NVME_H

/* Find an NVMe SSD on the PCI bus and register namespace 1 as "nvme0n1". */
void nvme_init(void);

#endif
