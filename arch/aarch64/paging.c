/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * paging.c — EL1 identity map for QEMU virt (step 14a/14b)
 *
 * TTBR0 → L1 (TG0=64K bring-up shortcut). Page tables in .pagetables.
 * TODO: switch to TG0=4K + L0→L1 for correct EL0 data AP (see docs/gaps.md).
 */
#include "paging.h"
#include "uart.h"

#include <stdint.h>

extern void arm64_mmu_configure(uint64_t tcr, uint64_t mair);
extern void arm64_mmu_enable(uint64_t ttbr0);

#define PTE_TYPE_BLOCK  0x1u
#define PTE_AF          (1ull << 10)
#define PTE_SH_INNER    (3ull << 8)
#define PTE_USER_RW     ((1ull << 6) | (1ull << 7))

#define MAIR_ATTR_NORM  0xffull
#define MAIR_ATTR_DEV   0x00ull

#define L1_SHIFT  30
#define L1_SIZE   (1ull << L1_SHIFT)

static uint64_t l1_table[512]
    __attribute__((section(".pagetables"), __aligned__(4096)));

static uint64_t l1_block_pte(uint64_t pa, unsigned attr_idx, uint64_t extra)
{
    return (pa & ~((1ull << L1_SHIFT) - 1)) | PTE_TYPE_BLOCK | PTE_AF |
           ((uint64_t)attr_idx << 2) | PTE_SH_INNER | extra;
}

uint64_t arm64_tcr_el1(void)
{
    return (16ull << 0) | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 14) |
           (1ull << 30) | (1ull << 32);
}

void paging_init(uint64_t ram_base, uint64_t ram_size)
{
    uint64_t tcr = arm64_tcr_el1();
    unsigned ram_l1 = (unsigned)(ram_base >> L1_SHIFT);
    unsigned ram_l1_end =
        (unsigned)((ram_base + ram_size + L1_SIZE - 1) >> L1_SHIFT);

    (void)ram_size;

    for (unsigned i = 0; i < 512; i++)
        l1_table[i] = 0;

    l1_table[0] = l1_block_pte(0, 1, 0);

    for (unsigned i = ram_l1; i < ram_l1_end && i < 512; i++)
        l1_table[i] = l1_block_pte((uint64_t)i << L1_SHIFT, 0, PTE_USER_RW);

    arm64_mmu_configure(tcr, (MAIR_ATTR_NORM << 0) | (MAIR_ATTR_DEV << 8));
    arm64_mmu_enable((uint64_t)(uintptr_t)l1_table);

    uart_puts("  MMU: on (TTBR0 identity, 1 GiB blocks, EL0 user)\n");
}
