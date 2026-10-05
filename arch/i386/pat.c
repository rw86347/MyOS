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
#define BIG_PAGE   0x400000u

/* Kernel addresses for write-combined device memory: 128 MiB, the top of
   the address space. Enough for a 64 MiB VRAM bar and more. */
#define WC_WINDOW     0xF8000000u
#define WC_WINDOW_END 0x00000000u       /* wraps: the end of the 4 GiB space */

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
 * Map [phys, phys + size) write-combined. Whole 4 MiB pieces get 4 MiB
 * pages (one TLB entry covers 4 MiB of screen — a 1920x1200 picture is
 * 9 MiB, so 3 entries instead of 2250), the rest 4 KiB pages. We never
 * round a big page past the end of the range: memory after it may be
 * another device's registers, and WC allows the CPU to read ahead
 * speculatively.
 */
volatile void *ioremap_wc(uint32_t phys, uint32_t size)
{
    return ioremap_wc64(phys, size);
}

volatile void *ioremap_wc64(uint64_t phys, uint32_t size)
{
    static uint32_t next = WC_WINDOW;
    uint32_t flags = PTE_WRITABLE | wc_flags;
    uint32_t off, len, va, done;

    if (!size)
        return NULL;

    /* Above 4 GiB a 32-bit PTE cannot store the address. 4 MiB PSE-36
       pages can (bits 39:32 live in the directory entry). */
    if (phys > 0xFFFFFFFFull || phys + size - 1 > 0xFFFFFFFFull) {
        uint64_t start, end;

        if (!cpu_has(CPU_PSE36))
            return NULL;
        start = phys & ~((uint64_t)BIG_PAGE - 1);
        end = (phys + size + BIG_PAGE - 1) & ~((uint64_t)BIG_PAGE - 1);
        len = (uint32_t)(end - start);
        off = (uint32_t)(phys - start);
        va = (next & ~(BIG_PAGE - 1)) | (uint32_t)(start & (BIG_PAGE - 1));
        if (va < next)
            va += BIG_PAGE;
        if (va < WC_WINDOW || len > WC_WINDOW_END - va)
            return NULL;
        for (done = 0; done < len; done += BIG_PAGE) {
            if (vmm_map_big(va + done, start + done, flags) < 0)
                return NULL;
        }
        next = va + len;
        return (volatile void *)(va + off);
    }

    off = (uint32_t)phys & (PAGE_SIZE - 1);
    {
        uint32_t start = (uint32_t)phys - off;
        len = (off + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        va = (next & ~(BIG_PAGE - 1)) | (start & (BIG_PAGE - 1));
        if (va < next)
            va += BIG_PAGE;
        if (va < WC_WINDOW || len > WC_WINDOW_END - va)
            return NULL;
        for (done = 0; done < len; ) {
            uint32_t v = va + done, p = start + done;
            if (!((v | p) & (BIG_PAGE - 1)) && len - done >= BIG_PAGE) {
                if (vmm_map_big(v, p, flags) < 0)
                    return NULL;
                done += BIG_PAGE;
            } else {
                if (vmm_map(v, p, flags) < 0)
                    return NULL;
                done += PAGE_SIZE;
            }
        }
        next = va + len;
        return (volatile void *)(va + off);
    }
}
