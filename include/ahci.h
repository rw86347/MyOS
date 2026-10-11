/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef AHCI_H
#define AHCI_H

/* Find an AHCI SATA controller on the PCI bus and register each disk on it
   as a block device named sda, sdb, ... */
void ahci_init(void);

#endif
