/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * mmu.S — turn on EL1 stage-1 translation (TTBR0_EL1, 4 KiB granule)
 */
    .section .text

    .global arm64_mmu_configure
    .type   arm64_mmu_configure, %function
/* void arm64_mmu_configure(uint64_t tcr, uint64_t mair); */
arm64_mmu_configure:
    msr     MAIR_EL1, x1
    msr     TCR_EL1, x0
    isb
    ret

    .global arm64_mmu_enable
    .type   arm64_mmu_enable, %function
/* void arm64_mmu_enable(uint64_t ttbr0); */
arm64_mmu_enable:
    msr     TTBR0_EL1, x0
    dsb     sy
    tlbi    vmalle1
    dsb     sy
    isb
    mrs     x1, SCTLR_EL1
    bic     x1, x1, #(1 << 4)       /* SA0 — EL0 SP alignment (bring-up) */
    bic     x1, x1, #(1 << 19)      /* WXN — stores to executable pages */
    orr     x1, x1, #(1 << 0)       /* M — MMU (I/D caches stay off for now) */
    msr     SCTLR_EL1, x1
    isb
    ret
