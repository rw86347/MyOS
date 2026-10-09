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
 * Address spaces (step 8): every process has its own page directory. Its
 * top quarter (0xC0000000 and up, entries 768-1023) is a copy of the
 * kernel's, so the kernel is mapped identically everywhere; the bottom three
 * quarters are the process's own. All kernel page tables exist from boot
 * on (the heap's are made in vmm_init), so those copied entries never go
 * stale. Switching processes = loading CR3 with another directory; kernel
 * mappings are "global" and stay in the TLB.
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
#include <serial.h>
#include <string.h>
#include <thread.h>

#define PDE_INDEX(va)  ((va) >> 22)             /* top 10 bits */
#define PTE_INDEX(va)  (((va) >> 12) & 0x3FF)   /* middle 10 bits */
#define FRAME(e)       ((e) & ~0xFFFu)          /* address part of an entry */
#define BIG_PAGE       0x400000u                /* 4 MiB */
#define IS_BIG(pde)    (((pde) & (PTE_PRESENT | PTE_PS)) == (PTE_PRESENT | PTE_PS))

extern uint32_t boot_page_directory[1024];      /* from entry.asm */
static uint32_t *kernel_pd = boot_page_directory;
static uint32_t *cur_pd = boot_page_directory;  /* the address space in CR3 */
static uint32_t cur_pd_phys;
static uint32_t global_bit;     /* PTE_GLOBAL if the CPU has it, else 0 */

#define USER_PDES  PDE_INDEX(KERNEL_VBASE)       /* entries 0-767: user space */

/* User addresses live in the current address space, kernel ones in the
   kernel's directory (whose tables every address space shares). */
static uint32_t *pd_for(uint32_t va)
{
    return va >= KERNEL_VBASE ? kernel_pd : cur_pd;
}

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
    cur_pd_phys = V2P(kernel_pd);


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

    /* 5. Page tables for the whole kernel heap and stack range, now, so
       that every process's copy of the kernel's directory already points
       at them. (After step 4: they're empty, nothing to mark global.) */
    _Static_assert(KHEAP_END == KSTACK_START, "one run of tables for both");
    for (uint32_t va = KHEAP_START; va < KSTACK_END; va += BIG_PAGE) {
        uint32_t pt = pmm_alloc();
        memset(P2V(pt), 0, PAGE_SIZE);
        kernel_pd[PDE_INDEX(va)] = pt | PTE_PRESENT | PTE_WRITABLE;
    }
}

/* Find the page table entry for `va`. If its page table doesn't exist yet,
   create it when `create` is set (else return NULL). */
static uint32_t *get_pte(uint32_t va, int create, uint32_t flags)
{
    uint32_t *pde = &pd_for(va)[PDE_INDEX(va)];

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

int vmm_map_big(uint32_t va, uint64_t pa, uint32_t flags)
{
    uint32_t *pde = &kernel_pd[PDE_INDEX(va)];
    uint32_t hi = (uint32_t)(pa >> 32);

    if (va < KERNEL_VBASE || ((va | (uint32_t)pa) & (BIG_PAGE - 1))
        || (pa & (BIG_PAGE - 1)) || (*pde & PTE_PRESENT))
        return -1;
    /* PSE-36: PDE bits 20:13 hold physical bits 39:32. Same encoding as
       the original 36-bit form (bits 16:13 = 35:32) for addresses < 64 GiB. */
    if (hi) {
        if (!cpu_has(CPU_PSE36) || hi > 0xFF)
            return -1;
    }
    *pde = ((uint32_t)pa & ~(BIG_PAGE - 1)) | ((hi & 0xFF) << 13)
         | (flags & 0xFFF) | PTE_PRESENT | PTE_PS | global_bit;
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
    uint32_t pde = pd_for(va)[PDE_INDEX(va)];
    if (IS_BIG(pde))            /* top 10 bits from the entry, 22 from va */
        return (pde & ~(BIG_PAGE - 1)) | (va & (BIG_PAGE - 1));

    uint32_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;
    return FRAME(*pte) | (va & 0xFFF);
}

uint32_t vmm_get_flags(uint32_t va)
{
    uint32_t *pte = get_pte(va, 0, 0);
    return pte ? (*pte & 0xFFF) : 0;
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

void vmm_unmap_range(uint32_t start, uint32_t end)
{
    for (uint32_t va = start & ~0xFFFu; va < end; va += PAGE_SIZE) {
        uint32_t *pde = &cur_pd[PDE_INDEX(va)];
        if (!(*pde & PTE_PRESENT)) {
            va |= BIG_PAGE - PAGE_SIZE;         /* skip the whole empty 4 MiB */
            continue;
        }
        uint32_t *pte = &((uint32_t *)P2V(FRAME(*pde)))[PTE_INDEX(va)];
        if (*pte & PTE_PRESENT) {
            if (!(*pte & PTE_SHARED))
                pmm_free(FRAME(*pte));          /* drops one owner */
            *pte = 0;
            invlpg(va);
        }
    }
}

/* --- address spaces ---------------------------------------------------- */

uint32_t vmm_pd_create(void)
{
    uint32_t phys = pmm_alloc();
    if (!phys)
        return 0;
    uint32_t *pd = P2V(phys);
    memset(pd, 0, USER_PDES * 4);                           /* user: empty */
    memcpy(pd + USER_PDES, kernel_pd + USER_PDES, (1024 - USER_PDES) * 4);
    return phys;
}

uint32_t vmm_pd_current(void)
{
    return cur_pd_phys;
}

/* A page directory that can't be right: stop, and say who wanted it. */
static void __attribute__((noreturn, noinline)) bad_table(const char *what, uint32_t phys)
{
    struct thread *t = thread_current();
    kprintf("\n*** PAGE TABLE CHECK: %s %08lx (thread \"%s\", called from %08lx) ***\n",
            what, (unsigned long)phys, t ? t->name : "?",
            (unsigned long)(uintptr_t)__builtin_return_address(0));
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* The kernel half of every directory is the kernel's (vmm_pd_create copies
   it). Two entries set at boot and never changed stand for it: the start
   of the direct map and the heap. A directory without them is freed or
   written over, and loading it would take the kernel away under the CPU —
   a triple fault, a reset, nothing said. Two compares. */
static int kernel_half_ok(uint32_t phys)
{
    const uint32_t *pd = P2V(phys);
    return pd[PDE_INDEX(KERNEL_VBASE)] == kernel_pd[PDE_INDEX(KERNEL_VBASE)] &&
           pd[PDE_INDEX(KHEAP_START)] == kernel_pd[PDE_INDEX(KHEAP_START)];
}

void vmm_pd_switch(uint32_t phys)
{
    if (phys == cur_pd_phys)
        return;                 /* same address space: keep the TLB */
    if (!kernel_half_ok(phys))
        bad_table("switch to a broken or freed address space", phys);
    __asm__ volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
    cur_pd = P2V(phys);
    cur_pd_phys = phys;
}

void vmm_pd_destroy(uint32_t phys)
{
    if (phys == cur_pd_phys || !kernel_half_ok(phys))
        bad_table(phys == cur_pd_phys ? "freeing the address space in use"
                                      : "freeing a broken or freed address space", phys);
    uint32_t *pd = P2V(phys);
    for (uint32_t i = 0; i < USER_PDES; i++) {
        if (!(pd[i] & PTE_PRESENT))
            continue;
        uint32_t *pt = P2V(FRAME(pd[i]));
        for (int j = 0; j < 1024; j++)
            if ((pt[j] & PTE_PRESENT) && !(pt[j] & PTE_SHARED))
                pmm_free(FRAME(pt[j]));         /* drops one owner */
        pmm_free(FRAME(pd[i]));                 /* the page table */
    }
    /* Unhook the kernel half: a switch to this directory after the free
       (until the page is reused) then fails the check above, loudly. */
    pd[PDE_INDEX(KERNEL_VBASE)] = pd[PDE_INDEX(KHEAP_START)] = 0;
    pmm_free(phys);
}

uint32_t vmm_pd_fork(void)
{
    uint32_t child_phys = vmm_pd_create();
    if (!child_phys)
        return 0;
    uint32_t *child = P2V(child_phys);

    for (uint32_t i = 0; i < USER_PDES; i++) {
        if (!(cur_pd[i] & PTE_PRESENT))
            continue;
        uint32_t pt_phys = pmm_alloc();
        if (!pt_phys) {
            vmm_pd_destroy(child_phys);
            return 0;
        }
        uint32_t *src = P2V(FRAME(cur_pd[i])), *dst = P2V(pt_phys);
        for (int j = 0; j < 1024; j++) {
            uint32_t e = src[j];
            if ((e & PTE_PRESENT) && !(e & PTE_SHARED)) {
                if ((e & PTE_WRITABLE) && !(e & PTE_MAPSHARED)) {   /* share it until someone writes */
                    e = (e & ~PTE_WRITABLE) | PTE_COW;
                    src[j] = e;
                }
                pmm_ref(FRAME(e));              /* one more owner */
            }
            dst[j] = e;
        }
        child[i] = pt_phys | (cur_pd[i] & 0xFFF);
    }
    flush_tlb();                /* we took write access away in our own tables */
    return child_phys;
}

int vmm_cow_fault(uint32_t va)
{
    uint32_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW))
        return -1;
    uint32_t old = FRAME(*pte), flags = (*pte & 0xFFF & ~PTE_COW) | PTE_WRITABLE;
    if (pmm_shared(old)) {
        /* Others still use the page: make our own copy. */
        uint32_t copy = pmm_alloc();
        if (!copy)
            return -1;
        memcpy(P2V(copy), P2V(old), PAGE_SIZE);
        *pte = copy | flags;
        pmm_free(old);                          /* one owner less */
    } else {
        *pte = old | flags;                     /* last owner: just take it */
    }
    invlpg(va & ~0xFFFu);
    return 0;
}
