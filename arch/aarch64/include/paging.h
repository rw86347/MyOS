/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_PAGING_H
#define ARCH_AARCH64_PAGING_H

#include <stdint.h>

void paging_init(uintptr_t dtb, uint64_t ram_base, uint64_t ram_size);

/* TCR_EL1 for 48-bit VA, 4 KiB granule, inner WB (matches Linux virt bring-up). */
uint64_t arm64_tcr_el1(void);

/* Per-process page tables: same VAs, private image/stack/mmap PAs. */
struct arm64_as {
    uint64_t l0_pa, l1_pa, l2_low_pa, l2_ram_pa;
    uint64_t image_pa, stack_pa, mmap_pa;
    uint64_t mmap_size;
    int refs;
    int owns_pages;
};

void paging_as_wrap_boot(struct arm64_as *as);
int  paging_as_fork(struct arm64_as *dst, struct arm64_as *src);
int  paging_as_fresh(struct arm64_as *as);
void paging_as_release(struct arm64_as *as);
void paging_as_activate(struct arm64_as *as);

#endif
