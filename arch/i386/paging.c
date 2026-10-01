/*
 * paging.c — x86 page tables (virtual memory)
 *
 * entry.asm turned paging on with a temporary setup that maps only the first
 * 4 MiB of RAM (at 0 and at 0xC0000000). Here we finish the job:
 *
 *   1. map ALL usable RAM (up to 768 MiB) into the direct map at 0xC0000000,
 *      using 4 MiB pages: no page tables needed, few TLB entries used,
 *   2. remove the identity map at 0, so a NULL pointer access page-faults,
 *   3. offer vmm_map()/vmm_unmap() to map single pages anywhere (used by the
 *      kernel heap now, and by user programs from step 5).
 *
 * Page directory / page table entries are 32 bits:
 *   bits 31-12: physical address of the page (or of the next-level table)
 *   bit 7 PS (directory entries only) this entry IS a 4 MiB page, no table
 *   bit 2 U  user may access (else kernel only)
 *   bit 1 W  writable
 *   bit 0 P  present (if 0, any access -> page fault, exception 14)
 *
 * To edit a page table we need a VIRTUAL address for it. Every page table is
 * a frame from pmm_alloc(), and all of RAM is in the direct map, so the
 * table for physical address `pa` is simply at P2V(pa).
 */
#include <cpu.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>

#define PDE_INDEX(va)  ((va) >> 22)             /* top 10 bits */
#define PTE_INDEX(va)  (((va) >> 12) & 0x3FF)   /* middle 10 bits */
#define FRAME(e)       ((e) & ~0xFFFu)          /* address part of an entry */
#define BIG_PAGE       0x400000u                /* 4 MiB */
#define IS_BIG(pde)    (((pde) & (PTE_PRESENT | PTE_PS)) == (PTE_PRESENT | PTE_PS))

extern uint32_t boot_page_directory[1024];      /* from entry.asm */
static uint32_t *kernel_pd = boot_page_directory;
static uint32_t global_bit;     /* PTE_GLOBAL if the CPU has it, else 0 */

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
       (The first 4 MiB are already mapped by entry.asm.) Every whole 4 MiB
       of RAM becomes one big page: one directory entry, no page table —
       even 768 MiB takes no memory at all to map. */
    uint32_t ram_end = pmm_end();
    for (uint32_t pa = BIG_PAGE; pa < ram_end; pa += BIG_PAGE) {
        uint32_t *pde = &kernel_pd[PDE_INDEX(KERNEL_VBASE + pa)];
        if (pa + BIG_PAGE <= ram_end) {
            *pde = pa | PTE_PRESENT | PTE_WRITABLE | PTE_PS;
            continue;
        }
        /* The last, partial 4 MiB: map it page by page, so we don't map
           addresses past the end of RAM (they may be device memory). The
           table comes from pmm_alloc(), which hands out the lowest free
           frame — already reachable through the big pages above. */
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
        *pde = pt_phys | PTE_PRESENT | PTE_WRITABLE;
    }

    /* 2. Drop the identity map. From now on, addresses below 0xC0000000
       are free for user programs, and NULL is not mapped. */
    kernel_pd[0] = 0;
    flush_tlb();

    /* 3. Write protection also for the kernel (CR0.WP). Without it, ring 0
       ignores the read-only bit, and a syscall could be tricked into
       writing to a program's read-only pages (its code). */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0 | (1u << 16)) : "memory");

    /* 4. Global pages (Pentium Pro and later). The TLB caches recent
       translations, and loading CR3 — which we'll do on every switch to
       another process (step 8) — throws them all away. Kernel mappings are
       the same in every process, so we mark them "global": the CPU keeps
       those across CR3 loads. Fewer TLB misses after every switch. */
    if (cpu_has(CPU_PGE)) {
        global_bit = PTE_GLOBAL;
        for (uint32_t i = PDE_INDEX(KERNEL_VBASE); i < 1024; i++) {
            if (IS_BIG(kernel_pd[i]))
                kernel_pd[i] |= PTE_GLOBAL;
            else if (kernel_pd[i] & PTE_PRESENT) {      /* a page table */
                uint32_t *pt = P2V(FRAME(kernel_pd[i]));
                for (int j = 0; j < 1024; j++)
                    if (pt[j] & PTE_PRESENT)
                        pt[j] |= PTE_GLOBAL;
            }
        }
        uint32_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= 1 << 7;                      /* CR4.PGE (also flushes the TLB) */
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
}

/* Find the page table entry for `va`. If its page table doesn't exist yet,
   create it when `create` is set (else return NULL). */
static uint32_t *get_pte(uint32_t va, int create, uint32_t flags)
{
    uint32_t *pde = &kernel_pd[PDE_INDEX(va)];

    if (IS_BIG(*pde))
        return NULL;            /* a 4 MiB page has no page table */
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
    if (va >= KERNEL_VBASE)
        flags |= global_bit;                    /* kernel mappings are global */
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
    uint32_t pde = kernel_pd[PDE_INDEX(va)];
    if (IS_BIG(pde))            /* top 10 bits from the entry, 22 from va */
        return (pde & ~(BIG_PAGE - 1)) | (va & (BIG_PAGE - 1));

    uint32_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;
    return FRAME(*pte) | (va & 0xFFF);
}

int vmm_protect(uint32_t va, uint32_t flags)
{
    uint32_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;
    *pte = FRAME(*pte) | (flags & 0xFFF) | PTE_PRESENT;
    invlpg(va);
    return 0;
}

void vmm_unmap_user_all(void)
{
    for (uint32_t i = 0; i < PDE_INDEX(KERNEL_VBASE); i++) {
        uint32_t pde = kernel_pd[i];
        if (!(pde & PTE_PRESENT))
            continue;
        uint32_t *pt = P2V(FRAME(pde));
        for (int j = 0; j < 1024; j++)
            if ((pt[j] & PTE_PRESENT) && !(pt[j] & PTE_SHARED))
                pmm_free(FRAME(pt[j]));
        kernel_pd[i] = 0;
        pmm_free(FRAME(pde));                   /* the page table itself */
    }
    flush_tlb();
}
