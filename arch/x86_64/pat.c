/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pat.c — write-combining memory for framebuffers (step 11b)
 *
 * Every page has a "memory type" that says how the CPU caches it:
 *
 *   WB  write-back       normal RAM: reads and writes go to the cache
 *   UC  uncached         device registers: every access goes to the device,
 *                        one by one, in order
 *   WC  write-combining  writes are collected in a small buffer (a 64-byte
 *                        line on the Pentium 4) and sent as one burst;
 *                        reads are uncached
 *
 * A framebuffer is written a lot and almost never read, so WC is the type
 * for it: filling a line of pixels uncached sends one bus transaction per
 * 4-byte pixel, WC sends one per 16 pixels. On old PCs that is several
 * times faster.
 *
 * The page table has two bits for this, PWT and PCD. Without PAT they only
 * choose WB, WT (write-through), UC- or UC. PAT (Pentium III and later,
 * also the Pentium M and Pentium 4) turns the two bits into an index into
 * an 8-entry table in MSR 0x277, whose entries we may change. We change
 * entry 1 (PWT set, PCD clear) from WT, which nothing uses, to WC — the
 * same choice Linux makes. A WC entry in PAT wins over the MTRRs, which
 * the BIOS sets to UC for the video memory.
 *
 * Without PAT (a Pentium II), framebuffers are mapped uncached: correct,
 * just slower.
 */
#include <cpu.h>
#include <linux/io.h>
#include <memlayout.h>
#include <paging.h>
#include <pci.h>
#include <stddef.h>

#define MSR_PAT    0x277
#define PAT_WC     0x01                 /* memory type numbers, as in Intel's manual */
#define BIG_PAGE   0x200000ull          /* a 2 MiB page (paging.c map_2mb) */

static uint32_t wc_flags = PTE_PCD;     /* uncached until pat_init */

void pat_init(void)
{
    if (!cpu_has(CPU_PAT))
        return;
    uint64_t pat = rdmsr(MSR_PAT);
    pat = (pat & ~(0xFFull << 8)) | ((uint64_t)PAT_WC << 8);    /* entry 1 */
    /* Nothing uses entry 1 yet (no page has PWT set), so no cached copy of
       a page can have the old type; flushing the caches is enough. */
    __asm__ volatile ("wbinvd" ::: "memory");
    wrmsr(MSR_PAT, pat);
    wc_flags = PTE_PWT;
}

uint32_t pte_write_combining(void)
{
    return wc_flags;
}

volatile void *ioremap(uint32_t phys, uint32_t size)
{
    return pci_map_mmio(phys, size);
}

/*
 * Map [phys, phys + size) write-combined, in the window from WC_BASE
 * (memlayout.h) to the end of memory. The virtual address is chosen with
 * the same offset inside a 2 MiB page as the physical one, so every whole,
 * aligned 2 MiB piece gets a 2 MiB page (one TLB entry covers 2 MiB of
 * screen — a 1920x1200 picture is 9 MiB, so 5 entries instead of 2250); the
 * ragged ends get 4 KiB pages. We never round a big page past the end of
 * the range: memory after it may be another device's registers, and WC
 * lets the CPU read ahead speculatively. Any physical address works: a
 * page-table entry holds 52 bits.
 */
volatile void *ioremap_wc(uint32_t phys, uint32_t size)
{
    return ioremap_wc64(phys, size);
}

volatile void *ioremap_wc64(uint64_t phys, uint32_t size)
{
    static uintptr_t next = WC_BASE;
    uint32_t flags = PTE_WRITABLE | wc_flags;

    if (!size)
        return NULL;
    uint64_t start = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uintptr_t va = (next & ~(BIG_PAGE - 1)) | (start & (BIG_PAGE - 1));
    if (va < next)
        va += BIG_PAGE;
    if (end - start > 0 - (uint64_t)va)     /* past the top of memory */
        return NULL;

    uint64_t pa = start;
    uintptr_t v = va;
    while (pa < end) {
        int big = !(pa & (BIG_PAGE - 1)) && end - pa >= BIG_PAGE;
        if (big ? vmm_map_big(v, pa, flags) : vmm_map(v, pa, flags))
            return NULL;
        pa += big ? BIG_PAGE : PAGE_SIZE;
        v += big ? BIG_PAGE : PAGE_SIZE;
    }
    next = v;
    return (volatile void *)(va + (phys - start));
}
