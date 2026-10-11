/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * paging.c — EL1 identity map for QEMU virt (step 14a/14b)
 *
 * TTBR0 → L0 → L1, TG0=4 KiB. Two L2 tables of 2 MiB blocks:
 *   GiB 0 — virt MMIO (device, EL1), plus a 4 MiB alias so Linux ET_EXEC
 *           images at 0x400000 land on RAM (busybox.static).
 *   RAM  — kernel below ARM64_USER_MIN is EL1-only; identity user RAM is
 *           AP[2:1]=01 (bit 6) so EL0 can load/store.
 *
 * AP[2:1] (ARM D5): 00 EL1 RW; 01 EL0+EL1 RW; 11 is read-only (not "user RW").
 */
#include "paging.h"
#include "memlayout.h"
#include "arm64_pmm.h"
#include "uaccess.h"
#include "uart.h"

#include <stdint.h>
#include <string.h>

extern void arm64_mmu_configure(uint64_t tcr, uint64_t mair);
extern void arm64_mmu_enable(uint64_t ttbr0);

#define PTE_TYPE_BLOCK  0x1u
#define PTE_TYPE_TABLE  0x3u
#define PTE_AF          (1ull << 10)
#define PTE_SH_INNER    (3ull << 8)
#define PTE_UXN         (1ull << 54)
#define PTE_AP_EL0_RW   (1ull << 6)

#define MAIR_ATTR_NORM  0xffull
#define MAIR_ATTR_DEV   0x00ull

#define L1_SHIFT  30
#define L1_SIZE   (1ull << L1_SHIFT)
#define L2_SHIFT  21
#define L2_SIZE   (1ull << L2_SHIFT)

static uint64_t l0_table[512]
    __attribute__((section(".pagetables"), __aligned__(4096)));
static uint64_t l1_table[512]
    __attribute__((section(".pagetables"), __aligned__(4096)));
static uint64_t l2_ram[512]
    __attribute__((section(".pagetables"), __aligned__(4096)));
static uint64_t l2_low[512]
    __attribute__((section(".pagetables"), __aligned__(4096)));
static unsigned s_ram_l1;

static uint64_t block_pte(uint64_t pa, uint64_t mask, unsigned attr_idx, uint64_t extra)
{
    return (pa & ~mask) | PTE_TYPE_BLOCK | PTE_AF | PTE_SH_INNER |
           ((uint64_t)attr_idx << 2) | extra;
}

uint64_t arm64_tcr_el1(void)
{
    /* T0SZ=16 (48-bit VA), TG0=4K, EPD1 (no TTBR1), TG1=4K, IPS=40-bit. */
    return (16ull << 0) | (1ull << 8) | (1ull << 10) | (3ull << 12) |
           (16ull << 16) | (1ull << 23) | (2ull << 30) | (2ull << 32);
}

void paging_init(uint64_t ram_base, uint64_t ram_size)
{
    uint64_t tcr = arm64_tcr_el1();
    unsigned ram_l1 = (unsigned)(ram_base >> L1_SHIFT);
    uint64_t ram_end = ram_base + ram_size;

    s_ram_l1 = ram_l1;

    for (unsigned i = 0; i < 512; i++) {
        l0_table[i] = 0;
        l1_table[i] = 0;
        l2_ram[i] = 0;
        l2_low[i] = 0;
    }

    /* 0–1 GiB: virt MMIO (GIC, UART) as 2 MiB device blocks, EL1 only. */
    for (unsigned i = 0; i < 512; i++) {
        uint64_t pa = (uint64_t)i << L2_SHIFT;

        l2_low[i] = block_pte(pa, L2_SIZE - 1, 1, PTE_UXN);
    }
    /* VA 0x400000 → RAM: Alpine busybox.static is linked here. */
    {
        unsigned idx = (unsigned)(ARM64_USER_IMAGE_VA >> L2_SHIFT);
        unsigned nblk = (unsigned)(ARM64_USER_IMAGE_SIZE >> L2_SHIFT);

        for (unsigned k = 0; k < nblk; k++) {
            uint64_t pa = ARM64_USER_IMAGE_PA + ((uint64_t)k << L2_SHIFT);

            l2_low[idx + k] = block_pte(pa, L2_SIZE - 1, 0, PTE_AP_EL0_RW);
        }
    }
    l1_table[0] = ((uint64_t)(uintptr_t)l2_low) | PTE_TYPE_TABLE;

    for (unsigned i = 0; i < 512; i++) {
        uint64_t pa = ((uint64_t)ram_l1 << L1_SHIFT) + ((uint64_t)i << L2_SHIFT);
        uint64_t extra = PTE_UXN;

        if (pa >= ram_end)
            break;
        if (pa >= ARM64_USER_MIN)
            extra = PTE_AP_EL0_RW;
        l2_ram[i] = block_pte(pa, L2_SIZE - 1, 0, extra);
    }

    l1_table[ram_l1] = ((uint64_t)(uintptr_t)l2_ram) | PTE_TYPE_TABLE;
    l0_table[0] = ((uint64_t)(uintptr_t)l1_table) | PTE_TYPE_TABLE;

    arm64_mmu_configure(tcr, (MAIR_ATTR_NORM << 0) | (MAIR_ATTR_DEV << 8));
    arm64_mmu_enable((uint64_t)(uintptr_t)l0_table);

    uart_puts("  MMU: on (TTBR0 L0→L1→L2, 4K, EL0 user)\n");
}

static void map_low_block(uint64_t *l2, uint64_t va, uint64_t pa)
{
    unsigned idx = (unsigned)(va >> L2_SHIFT);

    l2[idx] = block_pte(pa, L2_SIZE - 1, 0, PTE_AP_EL0_RW);
}

static void map_ram_block(uint64_t *l2, uint64_t va, uint64_t pa)
{
    unsigned idx = (unsigned)((va - ARM64_RAM_BASE) >> L2_SHIFT);

    l2[idx] = block_pte(pa, L2_SIZE - 1, 0, PTE_AP_EL0_RW);
}

static void apply_user_maps(struct arm64_as *as)
{
    uint64_t *low = (uint64_t *)(uintptr_t)as->l2_low_pa;
    uint64_t *ram = (uint64_t *)(uintptr_t)as->l2_ram_pa;
    uint64_t o;

    for (o = 0; o < ARM64_USER_IMAGE_SIZE; o += L2_SIZE)
        map_low_block(low, ARM64_USER_IMAGE_VA + o, as->image_pa + o);
    map_ram_block(ram, ARM64_STACK_BLOCK, as->stack_pa);
    for (o = 0; o < as->mmap_size; o += L2_SIZE)
        map_ram_block(ram, ARM64_MMAP_BASE + o, as->mmap_pa + o);
}

static int alloc_pts(struct arm64_as *as)
{
    uint32_t a = arm64_pmm_alloc();
    uint32_t b = arm64_pmm_alloc();
    uint32_t c = arm64_pmm_alloc();
    uint32_t d = arm64_pmm_alloc();

    if (!a || !b || !c || !d)
        return -1;
    as->l0_pa = a;
    as->l1_pa = b;
    as->l2_low_pa = c;
    as->l2_ram_pa = d;
    return 0;
}

static void fill_from_boot(struct arm64_as *as)
{
    uint64_t *l0 = (uint64_t *)(uintptr_t)as->l0_pa;
    uint64_t *l1 = (uint64_t *)(uintptr_t)as->l1_pa;

    memcpy(l0, l0_table, 4096);
    memcpy(l1, l1_table, 4096);
    memcpy((void *)(uintptr_t)as->l2_low_pa, l2_low, 4096);
    memcpy((void *)(uintptr_t)as->l2_ram_pa, l2_ram, 4096);
    l1[0] = as->l2_low_pa | PTE_TYPE_TABLE;
    l1[s_ram_l1] = as->l2_ram_pa | PTE_TYPE_TABLE;
    l0[0] = as->l1_pa | PTE_TYPE_TABLE;
}

void paging_as_wrap_boot(struct arm64_as *as)
{
    memset(as, 0, sizeof *as);
    as->l0_pa = (uint64_t)(uintptr_t)l0_table;
    as->l1_pa = (uint64_t)(uintptr_t)l1_table;
    as->l2_low_pa = (uint64_t)(uintptr_t)l2_low;
    as->l2_ram_pa = (uint64_t)(uintptr_t)l2_ram;
    as->image_pa = ARM64_USER_IMAGE_PA;
    as->stack_pa = ARM64_STACK_BLOCK;
    as->mmap_pa = ARM64_MMAP_BASE;
    as->mmap_size = ARM64_MMAP_END - ARM64_MMAP_BASE;
    as->refs = 1;
    as->owns_pages = 0;
}

int paging_as_fresh(struct arm64_as *as)
{
    memset(as, 0, sizeof *as);
    if (alloc_pts(as) < 0)
        return -1;
    as->image_pa = arm64_pmm_alloc_contig(ARM64_USER_IMAGE_SIZE, L2_SIZE);
    as->stack_pa = arm64_pmm_alloc_contig(ARM64_STACK_BLOCK_SIZE, L2_SIZE);
    as->mmap_size = 0x400000ULL;
    as->mmap_pa = arm64_pmm_alloc_contig(as->mmap_size, L2_SIZE);
    if (!as->image_pa || !as->stack_pa || !as->mmap_pa)
        return -1;
    as->refs = 1;
    as->owns_pages = 1;
    fill_from_boot(as);
    apply_user_maps(as);
    memset((void *)(uintptr_t)as->image_pa, 0, ARM64_USER_IMAGE_SIZE);
    memset((void *)(uintptr_t)as->stack_pa, 0, ARM64_STACK_BLOCK_SIZE);
    memset((void *)(uintptr_t)as->mmap_pa, 0, (size_t)as->mmap_size);
    return 0;
}

int paging_as_fork(struct arm64_as *dst, struct arm64_as *src)
{
    if (paging_as_fresh(dst) < 0)
        return -1;
    memcpy((void *)(uintptr_t)dst->image_pa,
           (void *)(uintptr_t)ARM64_USER_IMAGE_VA, ARM64_USER_IMAGE_SIZE);
    memcpy((void *)(uintptr_t)dst->stack_pa,
           (void *)(uintptr_t)ARM64_STACK_BLOCK, ARM64_STACK_BLOCK_SIZE);
    memcpy((void *)(uintptr_t)dst->mmap_pa,
           (void *)(uintptr_t)ARM64_MMAP_BASE,
           dst->mmap_size < src->mmap_size ? (size_t)dst->mmap_size
                                           : (size_t)src->mmap_size);
    return 0;
}

void paging_as_release(struct arm64_as *as)
{
    if (!as || --as->refs > 0)
        return;
    if (!as->owns_pages)
        return;
    arm64_pmm_free((uint32_t)as->l0_pa);
    arm64_pmm_free((uint32_t)as->l1_pa);
    arm64_pmm_free((uint32_t)as->l2_low_pa);
    arm64_pmm_free((uint32_t)as->l2_ram_pa);
    arm64_pmm_free_contig(as->image_pa, ARM64_USER_IMAGE_SIZE);
    arm64_pmm_free_contig(as->stack_pa, ARM64_STACK_BLOCK_SIZE);
    arm64_pmm_free_contig(as->mmap_pa, as->mmap_size);
}

void paging_as_activate(struct arm64_as *as)
{
    if (!as)
        return;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(as->l0_pa) : "memory");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb sy");
}
