/*
 * paging.c — x86 page tables (virtual memory)
 *
 * entry.asm turned paging on with a temporary setup that maps only the first
 * 4 MiB of RAM (at 0 and at 0xC0000000). Here we finish the job:
 *
 *   1. map ALL usable RAM (up to 768 MiB) into the direct map at 0xC0000000,
 *   2. remove the identity map at 0, so a NULL pointer access page-faults,
 *   3. offer vmm_map()/vmm_unmap() to map single pages anywhere (used by the
 *      kernel heap now, and by user programs from step 5).
 *
 * Page directory / page table entries are 32 bits:
 *   bits 31-12: physical address of the page (or of the next-level table)
 *   bit 2 U  user may access (else kernel only)
 *   bit 1 W  writable
 *   bit 0 P  present (if 0, any access -> page fault, exception 14)
 *
 * To edit a page table we need a VIRTUAL address for it. Every page table is
 * a frame from pmm_alloc(), and all of RAM is in the direct map, so the
 * table for physical address `pa` is simply at P2V(pa).
 */
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>

#define PDE_INDEX(va)  ((va) >> 22)             /* top 10 bits */
#define PTE_INDEX(va)  (((va) >> 12) & 0x3FF)   /* middle 10 bits */
#define FRAME(e)       ((e) & ~0xFFFu)          /* address part of an entry */

extern uint32_t boot_page_directory[1024];      /* from entry.asm */
static uint32_t *kernel_pd = boot_page_directory;

/* Tell the CPU to forget a cached translation (the TLB is a cache of
   recently used page table entries; it does not notice our edits). */
static void invlpg(uint32_t va)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

/* Reloading CR3 flushes the whole TLB. */
static void flush_tlb(void)
{
    __asm__ volatile ("mov %%cr3, %%eax\n\tmov %%eax, %%cr3" : : : "eax", "memory");
}

void vmm_init(void)
{
    /* 1. Direct map: physical [4 MiB, end of RAM) -> virtual +0xC0000000.
       (The first 4 MiB are already mapped by entry.asm's boot table.)
       New page tables come from pmm_alloc(), which hands out the lowest
       free frames first: those are just above 1 MiB, inside the 4 MiB we
       can already reach — so we can fill each table in right away. Even
       768 MiB of RAM needs only 192 tables = 768 KiB. */
    uint32_t ram_end = pmm_end();
    for (uint32_t pa = 0x400000; pa < ram_end; pa += 0x400000) {
        uint32_t pt_phys = pmm_alloc();
        if (!pt_phys) {
            kprintf("vmm: out of memory building the direct map\n");
            break;
        }
        uint32_t *pt = P2V(pt_phys);
        for (int i = 0; i < 1024; i++) {
            uint32_t page = pa + i * PAGE_SIZE;
            pt[i] = page < ram_end ? page | PTE_PRESENT | PTE_WRITABLE : 0;
        }
        kernel_pd[PDE_INDEX(KERNEL_VBASE + pa)] = pt_phys | PTE_PRESENT | PTE_WRITABLE;
    }

    /* 2. Drop the identity map. From now on, addresses below 0xC0000000
       are free for user programs, and NULL is not mapped. */
    kernel_pd[0] = 0;
    flush_tlb();
}

/* Find the page table entry for `va`. If its page table doesn't exist yet,
   create it when `create` is set (else return NULL). */
static uint32_t *get_pte(uint32_t va, int create, uint32_t flags)
{
    uint32_t *pde = &kernel_pd[PDE_INDEX(va)];

    if (!(*pde & PTE_PRESENT)) {
        if (!create)
            return NULL;
        uint32_t pt_phys = pmm_alloc();
        if (!pt_phys)
            return NULL;
        memset(P2V(pt_phys), 0, PAGE_SIZE);     /* all entries "not present" */
        /* The directory entry gets the loosest permissions; the individual
           page entries decide the real ones. */
        *pde = pt_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }
    uint32_t *pt = P2V(FRAME(*pde));
    return &pt[PTE_INDEX(va)];
}

int vmm_map(uint32_t va, uint32_t pa, uint32_t flags)
{
    uint32_t *pte = get_pte(va, 1, flags);
    if (!pte)
        return -1;
    if (*pte & PTE_PRESENT)
        return -1;                              /* already mapped */
    *pte = FRAME(pa) | (flags & 0xFFF) | PTE_PRESENT;
    invlpg(va);
    return 0;
}

uint32_t vmm_unmap(uint32_t va)
{
    uint32_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;
    uint32_t pa = FRAME(*pte);
    *pte = 0;
    invlpg(va);
    return pa;
}

uint32_t vmm_translate(uint32_t va)
{
    uint32_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;
    return FRAME(*pte) | (va & 0xFFF);
}

int vmm_user_ok(uint32_t va, uint32_t len, int write)
{
    if (len == 0)
        return 1;
    uint32_t last = va + len - 1;
    if (last < va || last >= KERNEL_VBASE)
        return 0;               /* wraps around, or reaches into the kernel */

    uint32_t need = PTE_PRESENT | PTE_USER | (write ? PTE_WRITABLE : 0);
    for (uint32_t page = va & ~0xFFFu; ; page += PAGE_SIZE) {
        /* Both levels must allow it: the CPU checks the directory entry
           and the page table entry. */
        uint32_t pde = kernel_pd[PDE_INDEX(page)];
        uint32_t *pte = get_pte(page, 0, 0);
        if ((pde & need) != need || !pte || (*pte & need) != need)
            return 0;
        if (page >= (last & ~0xFFFu))
            return 1;
    }
}
