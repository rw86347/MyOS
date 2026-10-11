/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pmm.c — physical page allocator for ARM64 bring-up (step 14a)
 *
 * Identity-mapped only: frame addresses are physical. Same bitmap idea as
 * mm/pmm.c; the portable PMM hooks in once paging exists.
 */
#include "fdt.h"
#include "arm64_pmm.h"
#include "uart.h"

#include "memlayout.h"

extern char _start[];
extern char __kernel_end[];

#define MAX_FRAMES (ARM64_DIRECTMAP_LIMIT / PAGE_SIZE)

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t nframes;
static uint32_t free_frames;
static uint32_t search_from;

static int is_used(uint32_t f)
{
    return bitmap[f / 32] & (1u << (f % 32));
}

static void set_used(uint32_t f)
{
    bitmap[f / 32] |= 1u << (f % 32);
}

static void set_free(uint32_t f)
{
    bitmap[f / 32] &= ~(1u << (f % 32));
}

static void mark_one(uint32_t f, int make_free)
{
    if (f >= nframes)
        return;
    if (make_free && is_used(f)) {
        set_free(f);
        free_frames++;
    } else if (!make_free && !is_used(f)) {
        set_used(f);
        if (free_frames)
            free_frames--;
    }
}

static void mark_range(uint64_t base, uint64_t len, int make_free)
{
    uint64_t end = base + len;

    if (make_free) {
        base = (base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        end &= ~(uint64_t)(PAGE_SIZE - 1);
    } else {
        base &= ~(uint64_t)(PAGE_SIZE - 1);
        end = (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    }
    if (end > ARM64_DIRECTMAP_LIMIT)
        end = ARM64_DIRECTMAP_LIMIT;
    if (base >= end)
        return;

    for (uint64_t addr = base; addr < end; addr += PAGE_SIZE)
        mark_one((uint32_t)(addr / PAGE_SIZE), make_free);
}

void arm64_pmm_init(uintptr_t dtb)
{
    uint64_t base = 0, size = 0;

    for (uint32_t i = 0; i < MAX_FRAMES / 32; i++)
        bitmap[i] = 0xFFFFFFFFu;
    free_frames = 0;
    nframes = 0;

    if (fdt_first_ram(dtb, &base, &size) < 0) {
        base = 0x40000000UL;
        size = 256UL * 1024 * 1024;
    }

    /* Frame index = physical address / PAGE_SIZE (RAM may start at 0x40000000). */
    nframes = MAX_FRAMES;

    mark_range(base, size, 1);

    /* Everything below this RAM bank is not DRAM (MMIO, etc.). */
    if (base > 0)
        mark_range(0, base, 0);
    /* Kernel image (physical == linked address while MMU is off). */
    mark_range((uint64_t)(uintptr_t)_start, (uint64_t)(__kernel_end - _start), 0);
    mark_range(ARM64_USER_MIN, 0x200000ULL, 0);
    mark_range(ARM64_USER_IMAGE_PA, ARM64_USER_IMAGE_SIZE, 0);
    mark_range(ARM64_STACK_BLOCK, ARM64_STACK_BLOCK_SIZE, 0);
    mark_range(ARM64_MMAP_BASE, ARM64_MMAP_END - ARM64_MMAP_BASE, 0);
    mark_range(ARM64_INTERP_VA, ARM64_INTERP_SIZE, 0);
    search_from = (uint32_t)((uintptr_t)_start / PAGE_SIZE);
}

uint64_t arm64_pmm_alloc_contig(uint64_t bytes, uint64_t align)
{
    uint32_t need, step, f, i;

    if (!bytes)
        return 0;
    if (!align)
        align = PAGE_SIZE;
    need = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    step = (uint32_t)(align / PAGE_SIZE);
    if (!step)
        step = 1;
    for (f = 0; f + need <= nframes; f += step) {
        int ok = 1;

        for (i = 0; i < need; i++) {
            if (is_used(f + i)) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;
        for (i = 0; i < need; i++) {
            set_used(f + i);
            if (free_frames)
                free_frames--;
        }
        return (uint64_t)f * PAGE_SIZE;
    }
    return 0;
}

void arm64_pmm_free_contig(uint64_t pa, uint64_t bytes)
{
    uint64_t end = pa + bytes;

    pa &= ~(uint64_t)(PAGE_SIZE - 1);
    for (; pa < end; pa += PAGE_SIZE)
        arm64_pmm_free((uint32_t)pa);
}

static uint32_t take_frame(void)
{
    for (uint32_t f = search_from; f < nframes; f++) {
        if (!is_used(f)) {
            set_used(f);
            if (free_frames)
                free_frames--;
            return f * PAGE_SIZE;
        }
    }
    for (uint32_t f = 0; f < search_from; f++) {
        if (!is_used(f)) {
            set_used(f);
            if (free_frames)
                free_frames--;
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

uint32_t arm64_pmm_alloc(void)
{
    return take_frame();
}

void arm64_pmm_free(uint32_t phys)
{
    uint32_t f = phys / PAGE_SIZE;

    if (phys % PAGE_SIZE || f >= nframes || !is_used(f))
        return;
    set_free(f);
    free_frames++;
    if (f < search_from)
        search_from = f;
}

uint32_t arm64_pmm_free_frames(void)
{
    return free_frames;
}

uint32_t arm64_pmm_total_frames(void)
{
    return nframes;
}
