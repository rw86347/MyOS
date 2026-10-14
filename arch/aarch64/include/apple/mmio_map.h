/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_APPLE_MMIO_MAP_H
#define ARCH_AARCH64_APPLE_MMIO_MAP_H

#include <stdint.h>

#define ARM64_MMIO_L1_SHIFT 30
#define ARM64_MMIO_L2_SHIFT 21
#define ARM64_MMIO_L1_SIZE  (1ull << ARM64_MMIO_L1_SHIFT)
#define ARM64_MMIO_L2_SIZE  (1ull << ARM64_MMIO_L2_SHIFT)

static inline void arm64_mmio_block_indices(uint64_t pa, unsigned *l1_out, unsigned *l2_out)
{
    unsigned l1 = (unsigned)(pa >> ARM64_MMIO_L1_SHIFT);
    unsigned l2 = (unsigned)((pa & (ARM64_MMIO_L1_SIZE - 1)) >> ARM64_MMIO_L2_SHIFT);

    if (l1_out)
        *l1_out = l1;
    if (l2_out)
        *l2_out = l2;
}

static inline uint64_t arm64_mmio_block_align(uint64_t pa)
{
    return pa & ~(ARM64_MMIO_L2_SIZE - 1);
}

#endif
