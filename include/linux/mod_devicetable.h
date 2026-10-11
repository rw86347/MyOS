/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_MOD_DEVICETABLE_H
#define LINUX_MOD_DEVICETABLE_H

#include <linux/types.h>

/* One row of a PCI driver's id table. PCI_ANY_ID matches anything; a
   class row matches (device class & class_mask) == class. */
typedef unsigned long kernel_ulong_t;

struct pci_device_id {
    u32 vendor, device;
    u32 subvendor, subdevice;
    u32 class, class_mask;
    kernel_ulong_t driver_data;
};

#endif
