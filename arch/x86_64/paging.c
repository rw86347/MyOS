/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * paging.c — x86_64 four-level page tables
 *
 * A virtual address is split into four 9-bit table indexes and a 12-bit
 * offset (Intel SDM vol. 3, 4.5):
 *
 *   CR3 -> PML4[47:39] -> PDPT[38:30] -> PD[29:21] -> PT[20:12] -> 4 KiB page
 *
 * A PD entry with PTE_PS set is a 2 MiB page instead (no PT): that is what
 * the direct map and the kernel image use — 512 small pages' worth of RAM
 * in one TLB entry.
 *
 * Kernel and user (memlayout.h): user space is PML4 entries 0-255, the
 * kernel 256-511. The kernel uses only 256 (direct map) and 511 (image,
 * heap, MMIO, framebuffers); both are made in vmm_init and never change.
 * A new process's PML4 copies those two entries, so it points at the very
 * same lower tables — a kernel mapping made later (kmalloc growing, a
 * device mapped) is in every address space at once. Nothing to sync.
 *
 * Every table is a physical frame from pmm_alloc(), edited through the
 * direct map (P2V).
 */
#include <cpu.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <serial.h>
#include <string.h>
#include <thread.h>
#include <types.h>
#include <user.h>

#define PTE_ADDR(e)   ((uint64_t)(e) & 0x000FFFFFFFFFF000ull)
#define PTE_FLAGS(e)  ((uint32_t)(e) & 0xFFFu)
#define HUGE_2MB      0x200000ull
#define ENTRIES       512

#define PML4_INDEX(va)  (((uint64_t)(va) >> 39) & 0x1FFu)
#define PDPT_INDEX(va)  (((uint64_t)(va) >> 30) & 0x1FFu)
#define PD_INDEX(va)    (((uint64_t)(va) >> 21) & 0x1FFu)
#define PT_INDEX(va)    (((uint64_t)(va) >> 12) & 0x1FFu)
#define USER_PML4ES     256             /* entries 0-255: user space */

extern char __kernel_end[];

static uint64_t *kernel_pml4;
static uint64_t *cur_pml4;
static phys_addr_t cur_pml4_phys;
static uint32_t global_bit;

static void invlpg(uintptr_t va)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static void flush_tlb(void)
{
    __asm__ volatile ("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");
}

static uint64_t *table_at(phys_addr_t phys)
{
    return (uint64_t *)P2V(phys);
}

/* A new, empty table, linked into *entry. Tables in user space are marked
   user and writable; the last level (the page itself) decides. */
static uint64_t *new_table(uint64_t *entry, uintptr_t va)
{
    phys_addr_t phys = pmm_alloc();
    if (!phys)
        return NULL;
    memset(P2V(phys), 0, PAGE_SIZE);
    *entry = phys | PTE_PRESENT | PTE_WRITABLE | (va < USER_ADDR_END ? PTE_USER : 0);
    return table_at(phys);
}

/* The PD entry for `va` (its 2 MiB slot), making the PDPT and PD on the
   way if `create`. Kernel addresses are looked up in the kernel's own PML4,
   whose two entries every process shares. */
static uint64_t *get_pde(uintptr_t va, int create)
{
    uint64_t *pml4e = va >= KERNEL_SPACE_START ? &kernel_pml4[PML4_INDEX(va)]
                                               : &cur_pml4[PML4_INDEX(va)];
    uint64_t *pdpt;
    if (*pml4e & PTE_PRESENT)
        pdpt = table_at(PTE_ADDR(*pml4e));
    else if (!create || va >= KERNEL_SPACE_START)
        return NULL;    /* a new kernel PML4 entry would not reach other processes */
    else if (!(pdpt = new_table(pml4e, va)))
        return NULL;

    uint64_t *pdpte = &pdpt[PDPT_INDEX(va)];
    uint64_t *pd;
    if (*pdpte & PTE_PRESENT)
        pd = table_at(PTE_ADDR(*pdpte));
    else if (!create || !(pd = new_table(pdpte, va)))
        return NULL;
    return &pd[PD_INDEX(va)];
}

/* The page-table entry for `va`; NULL if there is none (or `va` is inside
   a 2 MiB page). */
static uint64_t *get_pte(uintptr_t va, int create)
{
    uint64_t *pde = get_pde(va, create);
    if (!pde || (*pde & PTE_PS))
        return NULL;
    uint64_t *pt;
    if (*pde & PTE_PRESENT)
        pt = table_at(PTE_ADDR(*pde));
    else if (!create || !(pt = new_table(pde, va)))
        return NULL;
    return &pt[PT_INDEX(va)];
}

/* Kernel pages are global: they stay in the TLB when CR3 changes. Never a
   user page — it would stay visible to the next process. */
static uint32_t global_for(uintptr_t va)
{
    return va >= KERNEL_SPACE_START ? global_bit : 0;
}

static int map_2mb(uintptr_t va, uint64_t pa, uint32_t flags)
{
    if ((va | pa) & (HUGE_2MB - 1))
        return -1;
    uint64_t *pde = get_pde(va, 1);
    if (!pde || (*pde & PTE_PRESENT))
        return -1;
    *pde = pa | PTE_PRESENT | PTE_PS | global_for(va) | (flags & 0xFFF);
    invlpg(va);
    return 0;
}

/* One of the kernel's two PML4 entries, with an empty PDPT. */
static void kernel_pml4_entry(uintptr_t va)
{
    if (!new_table(&kernel_pml4[PML4_INDEX(va)], va))
        kprintf("vmm: out of memory for the kernel's tables\n");
}

/*
 * Build the kernel's page tables and switch to them. Until now we ran on
 * entry.asm's boot tables (identity + direct map + image, the first 1 GiB);
 * the new ones have no identity map, so the bottom half is free for user
 * space. The frames used here come from the bottom of RAM, inside that
 * first 1 GiB, which the boot tables reach.
 */
void vmm_init(void)
{
    /* Global pages first, so the mappings below get the bit. */
    global_bit = 0;
    if (cpu_has(CPU_PGE)) {
        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4 | (1u << 7)) : "memory");
        global_bit = PTE_GLOBAL;
    }

    phys_addr_t pml4_phys = pmm_alloc();
    if (!pml4_phys) {
        kprintf("vmm: out of memory for PML4\n");
        return;
    }
    kernel_pml4 = table_at(pml4_phys);
    memset(kernel_pml4, 0, PAGE_SIZE);
    kernel_pml4_entry(DIRECTMAP_BASE);
    kernel_pml4_entry(KERNEL_VBASE);

    /* The direct map: all RAM, 2 MiB pages (a ragged end in 4 KiB ones). */
    uint64_t ram_end = pmm_end();
    if (ram_end > DIRECTMAP_LIMIT)
        ram_end = DIRECTMAP_LIMIT;
    uint64_t pa = 0;
    for (; pa + HUGE_2MB <= ram_end; pa += HUGE_2MB)
        map_2mb(DIRECTMAP_BASE + pa, pa, PTE_WRITABLE);
    for (; pa < ram_end; pa += PAGE_SIZE)
        vmm_map(DIRECTMAP_BASE + pa, pa, PTE_WRITABLE);
    /* The high zone: each RAM range above it, in whole 2 MiB pages (a
       range's ragged ends are firmware data next to RAM, never device
       registers: those sit far above the end of RAM). Only RAM is mapped,
       not the holes, so nothing can speculatively read a device. */
    phys_addr_t hs, he;
    for (int i = 0; pmm_high_range(i, &hs, &he); i++)
        for (pa = hs & ~(uint64_t)(HUGE_2MB - 1); pa < he; pa += HUGE_2MB)
            if (pa >= ram_end)
                map_2mb(DIRECTMAP_BASE + pa, pa, PTE_WRITABLE);     /* -1: mapped already */

    /* The kernel image (code, data, .bss): 2 MiB pages from physical 0. */
    uint64_t image_end = V2P(__kernel_end);
    for (pa = 0; pa < image_end; pa += HUGE_2MB)
        map_2mb(KERNEL_VBASE + pa, pa, PTE_WRITABLE);

    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    cur_pml4 = kernel_pml4;
    cur_pml4_phys = pml4_phys;

    /* CR0.WP: the kernel, too, faults on writing a read-only page — how a
       copy_to_user into a copy-on-write page gets its own copy. */
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0 | (1ull << 16)) : "memory");
}

int vmm_map(uintptr_t va, uint64_t pa, uint32_t flags)
{
    uint64_t *pte = get_pte(va, 1);
    if (!pte || (*pte & PTE_PRESENT))
        return -1;
    *pte = (pa & 0x000FFFFFFFFFF000ull) | PTE_PRESENT | global_for(va) | (flags & 0xFFF);
    invlpg(va);
    return 0;
}

/* A 2 MiB kernel page (framebuffers: pat.c). */
int vmm_map_big(uintptr_t va, uint64_t pa, uint32_t flags)
{
    if (va < KERNEL_SPACE_START)
        return -1;                      /* big pages are the kernel's only */
    return map_2mb(va, pa, flags);
}

phys_addr_t vmm_unmap(uintptr_t va)
{
    uint64_t *pte = get_pte(va, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;
    phys_addr_t pa = PTE_ADDR(*pte);
    *pte = 0;
    invlpg(va);
    return pa;
}

phys_addr_t vmm_translate(uintptr_t va)
{
    uint64_t *pde = get_pde(va, 0);
    if (!pde || !(*pde & PTE_PRESENT))
        return 0;
    if (*pde & PTE_PS)
        return PTE_ADDR(*pde) + (va & (HUGE_2MB - 1));
    uint64_t pte = table_at(PTE_ADDR(*pde))[PT_INDEX(va)];
    if (!(pte & PTE_PRESENT))
        return 0;
    return PTE_ADDR(pte) | (va & 0xFFF);
}

uint32_t vmm_get_flags(uintptr_t va)
{
    uint64_t *pte = get_pte(va, 0);
    return pte && (*pte & PTE_PRESENT) ? PTE_FLAGS(*pte) : 0;
}

int vmm_protect(uintptr_t va, uint32_t flags)
{
    uint64_t *pte = get_pte(va, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;
    *pte = PTE_ADDR(*pte) | PTE_PRESENT | global_for(va) | (flags & 0xFFF);
    invlpg(va);
    return 0;
}

/* Unmap user pages in [start, end) and drop our hold on their frames. Where
   a whole page table is missing, skip its 2 MiB at once: unmapping a big,
   mostly empty range costs the tables that exist, not the pages it spans. */
void vmm_unmap_range(uintptr_t start, uintptr_t end)
{
    uintptr_t va = start & ~(uintptr_t)0xFFF;
    while (va < end) {
        uint64_t *pde = get_pde(va, 0);
        if (!pde || !(*pde & PTE_PRESENT) || (*pde & PTE_PS)) {
            uintptr_t next = (va | (HUGE_2MB - 1)) + 1;     /* the next 2 MiB slot */
            if (next <= va)
                break;
            va = next;
            continue;
        }
        uint64_t *pt = table_at(PTE_ADDR(*pde));
        for (uint32_t i = PT_INDEX(va); i < ENTRIES && va < end; i++, va += PAGE_SIZE) {
            uint64_t e = pt[i];
            if (!(e & PTE_PRESENT))
                continue;
            if (!(e & PTE_SHARED))
                pmm_free(PTE_ADDR(e));      /* 64 bits: a program's page may be high */
            pt[i] = 0;
            invlpg(va);
        }
    }
}

/* --- address spaces ------------------------------------------------------ */

uint32_t vmm_pd_create(void)
{
    phys_addr_t phys = pmm_alloc();
    if (!phys)
        return 0;
    uint64_t *pml4 = table_at(phys);
    memset(pml4, 0, USER_PML4ES * 8);                       /* user: empty */
    memcpy(pml4 + USER_PML4ES, kernel_pml4 + USER_PML4ES,   /* kernel: shared */
           (ENTRIES - USER_PML4ES) * 8);
    return (uint32_t)phys;
}

uint32_t vmm_pd_current(void)
{
    return (uint32_t)cur_pml4_phys;
}

/* A page table that can't be right: stop, and say who wanted it. */
static void __attribute__((noreturn, noinline)) bad_table(const char *what, uint64_t phys)
{
    struct thread *t = thread_current();
    kprintf("\n*** PAGE TABLE CHECK: %s %lx (thread \"%s\", called from %lx) ***\n",
            what, (unsigned long)phys, t ? t->name : "?",
            (unsigned long)(uintptr_t)__builtin_return_address(0));
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* The kernel half of every address space is the kernel's own (two PML4
   entries, copied by vmm_pd_create, never changed). One that isn't is a
   freed table someone still points at, or one written over: loading it
   would pull the kernel's code and stacks out from under the CPU — a
   triple fault, a reset, nothing said. Two compares. */
static int kernel_half_ok(phys_addr_t phys)
{
    const uint64_t *t = table_at(phys);
    return t[PML4_INDEX(DIRECTMAP_BASE)] == kernel_pml4[PML4_INDEX(DIRECTMAP_BASE)] &&
           t[PML4_INDEX(KERNEL_VBASE)] == kernel_pml4[PML4_INDEX(KERNEL_VBASE)];
}

void vmm_pd_switch(uint32_t phys)
{
    if ((phys_addr_t)phys == cur_pml4_phys)
        return;                 /* same address space: keep the TLB */
    if (!kernel_half_ok(phys))
        bad_table("switch to a broken or freed address space", phys);
    __asm__ volatile ("mov %0, %%cr3" : : "r"((phys_addr_t)phys) : "memory");
    cur_pml4 = table_at(phys);
    cur_pml4_phys = phys;
}

/* Free a user table and everything under it: `level` 3 = PDPT, 2 = PD,
   1 = PT. A page in a PT loses one owner (it may be shared copy-on-write). */
static void free_tree(phys_addr_t table, int level)
{
    uint64_t *t = table_at(table);
    for (int i = 0; i < ENTRIES; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_PRESENT))
            continue;
        if (level > 1)
            free_tree(PTE_ADDR(e), level - 1);
        else if (!(e & PTE_SHARED))
            pmm_free(PTE_ADDR(e));
    }
    pmm_free(table);
}

void vmm_pd_destroy(uint32_t phys)
{
    if ((phys_addr_t)phys == cur_pml4_phys || !kernel_half_ok(phys))
        bad_table(phys == cur_pml4_phys ? "freeing the address space in use"
                                        : "freeing a broken or freed address space", phys);
    uint64_t *pml4 = table_at(phys);
    for (int i = 0; i < USER_PML4ES; i++)
        if (pml4[i] & PTE_PRESENT)
            free_tree(PTE_ADDR(pml4[i]), 3);
    /* Unhook the kernel half: a switch to this table after the free (until
       the page is reused) then fails the check above, loudly. */
    pml4[PML4_INDEX(DIRECTMAP_BASE)] = pml4[PML4_INDEX(KERNEL_VBASE)] = 0;
    pmm_free(phys);
}

/* Copy the user table `src` (`level` as in free_tree) for fork, into a new
   table hung in *dst_entry. In a PT, private writable pages become
   read-only + PTE_COW in BOTH copies and gain an owner; the first write by
   either side makes its own copy (vmm_cow_fault). On -1 (out of memory)
   the part built so far is linked in, so vmm_pd_destroy frees it. */
static int fork_tree(uint64_t *src, uint64_t *dst_entry, uint64_t src_entry, int level)
{
    phys_addr_t phys = pmm_alloc();
    if (!phys)
        return -1;
    uint64_t *dst = table_at(phys);
    memset(dst, 0, PAGE_SIZE);
    *dst_entry = phys | PTE_FLAGS(src_entry);
    for (int i = 0; i < ENTRIES; i++) {
        uint64_t e = src[i];
        if (!(e & PTE_PRESENT))
            continue;
        if (level > 1) {
            if (fork_tree(table_at(PTE_ADDR(e)), &dst[i], e, level - 1) < 0)
                return -1;
            continue;
        }
        if (!(e & PTE_SHARED)) {
            if ((e & PTE_WRITABLE) && !(e & PTE_MAPSHARED)) {   /* share it until someone writes */
                e = (e & ~(uint64_t)PTE_WRITABLE) | PTE_COW;
                src[i] = e;
            }
            pmm_ref(PTE_ADDR(e));                       /* one more owner */
        }
        dst[i] = e;
    }
    return 0;
}

#ifdef PT_AUDIT
#include <kheap.h>
#include <proc.h>
#include <mm.h>
/*
 * make PTAUDIT=1: when a program faults, check every process's page tables
 * against the page allocator. A frame a process maps must be in use, and a
 * private (copy-on-write) frame must have at least as many owners as there
 * are mappings of it — else it will be freed, and handed to someone else,
 * while still mapped. Runs at fault time only, so it can't change the
 * timing that leads up to a crash.
 */
static uint16_t *audit_maps;
static int audit_report;

static void audit_walk(int pid, uint64_t *pml4, int count, uint32_t want)
{
    for (int a = 0; a < USER_PML4ES; a++) {
        if (!(pml4[a] & PTE_PRESENT))
            continue;
        uint64_t *pdpt = table_at(PTE_ADDR(pml4[a]));
        for (int b = 0; b < 512; b++) {
            if (!(pdpt[b] & PTE_PRESENT) || (pdpt[b] & PTE_PS))
                continue;
            uint64_t *pd = table_at(PTE_ADDR(pdpt[b]));
            for (int c = 0; c < 512; c++) {
                if (!(pd[c] & PTE_PRESENT) || (pd[c] & PTE_PS))
                    continue;
                uint64_t *pt = table_at(PTE_ADDR(pd[c]));
                for (int d = 0; d < 512; d++) {
                    uint64_t e = pt[d];
                    if (!(e & PTE_PRESENT))
                        continue;
                    uint64_t va = ((uint64_t)a << 39) | ((uint64_t)b << 30) |
                                  ((uint64_t)c << 21) | ((uint64_t)d << 12);
                    phys_addr_t full = PTE_ADDR(e);
                    int own = pmm_owners(full);
                    if (own < 0)
                        continue;           /* device memory, or the high zone */
                    uint32_t phys = (uint32_t)full;   /* low: fits */
                    if (want) {             /* pass 2: who maps a bad frame */
                        if (phys == want)
                            kprintf("  audit:   pid %d va %lx flags %lx\n", pid,
                                    (unsigned long)va, (unsigned long)(e & 0xFFF));
                        continue;
                    }
                    if (own == 0 && audit_report++ < 20)
                        kprintf("  audit: pid %d va %lx maps FREE frame %x (flags %lx)\n",
                                pid, (unsigned long)va, phys, (unsigned long)(e & 0xFFF));
                    if (count && !(e & (PTE_SHARED | PTE_MAPSHARED)) && audit_maps[phys / PAGE_SIZE] < 0xFFFF)
                        audit_maps[phys / PAGE_SIZE]++;
                }
            }
        }
    }
}

void vmm_audit(void)
{
    uint32_t nf = pmm_end() / PAGE_SIZE;
    audit_maps = kmalloc(nf * sizeof *audit_maps);
    audit_report = 0;
    if (!audit_maps) {
        kprintf("  audit: no memory for the count\n");
        return;
    }
    memset(audit_maps, 0, nf * sizeof *audit_maps);
    int procs = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state != PROC_ALIVE || !p->mm || !p->mm->pd)
            continue;
        procs++;
        audit_walk(p->pid, table_at(p->mm->pd), 1, 0);
    }
    uint32_t low = 0, frames = 0;
    for (uint32_t f = 0; f < nf; f++) {
        if (!audit_maps[f])
            continue;
        frames++;
        int own = pmm_owners(f * PAGE_SIZE);
        if (own >= 0 && audit_maps[f] > own) {
            if (low++ < 5) {
                kprintf("  audit: frame %x is mapped %u times but has %d owner(s):\n",
                        f * PAGE_SIZE, audit_maps[f], own);
                for (int i = 0; i < PROC_MAX; i++) {
                    struct process *p = proc_at(i);
                    if (p->state == PROC_ALIVE && p->mm && p->mm->pd)
                        audit_walk(p->pid, table_at(p->mm->pd), 0, f * PAGE_SIZE);
                }
            }
        }
    }
    kprintf("  audit: %d processes, %u private frames mapped, %d free-but-mapped, "
            "%u with too few owners\n", procs, frames, audit_report, low);
    kfree(audit_maps);
    audit_maps = NULL;
}
#endif

uint32_t vmm_pd_fork(void)
{
    uint32_t child_phys = vmm_pd_create();
    if (!child_phys)
        return 0;
    uint64_t *child = table_at(child_phys);
    for (int i = 0; i < USER_PML4ES; i++) {
        if (!(cur_pml4[i] & PTE_PRESENT))
            continue;
        if (fork_tree(table_at(PTE_ADDR(cur_pml4[i])), &child[i], cur_pml4[i], 3) < 0) {
            flush_tlb();
            vmm_pd_destroy(child_phys);
            return 0;
        }
    }
    flush_tlb();                /* we took write access away in our own tables */
    return child_phys;
}

int vmm_cow_fault(uintptr_t va)
{
    uint64_t *pte = get_pte(va, 0);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW))
        return -1;
    phys_addr_t old = PTE_ADDR(*pte);
    uint64_t flags = (PTE_FLAGS(*pte) & ~(uint64_t)PTE_COW) | PTE_WRITABLE;
    if (pmm_shared(old)) {
        /* Others still use the page: make our own copy (a program's page:
           the high zone's, if there is one). */
        phys_addr_t copy = pmm_alloc_user();
        if (!copy)
            return -1;
        memcpy(P2V(copy), P2V(old), PAGE_SIZE);
        *pte = copy | flags;
        pmm_free(old);                          /* one owner less */
    } else {
        *pte = old | flags;                     /* last owner: just take it */
    }
    invlpg(va & ~(uintptr_t)0xFFF);
    return 0;
}
