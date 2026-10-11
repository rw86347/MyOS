/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_PAGING_H
#define ARCH_AARCH64_PAGING_H

#include <stdint.h>

void paging_init(uint64_t ram_base, uint64_t ram_size);

/* TCR_EL1 for 48-bit VA, 4 KiB granule, inner WB (matches Linux virt bring-up). */
uint64_t arm64_tcr_el1(void);

#endif
