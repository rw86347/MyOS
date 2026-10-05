/*
 * paging.c — x86_64 four-level page tables (phase B bootstrap)
 *
 * UEFI leaves long mode with *firmware* page tables (usually identity). This
 * file builds *our* tables and loads CR3 before user processes or a higher-half
 * kernel layout.
 *
 * REVIEW checklist (for later Claude pass):
 *   - Move kernel to upper canonical (KERNEL_VBASE) and drop low identity map.
 *   - Split vmm_pd_create() so user PML4 entries don't share leaf tables with
 *     the kernel direct map (today KERNEL_VBASE==0 forces a shared low half).
 *   - 1 GiB pages for direct map where RAM is aligned (fewer TLB entries).
 *   - vmm_map_big() for GOP above 4 GiB without i386 PSE-36 tricks.
 *
 * 4-level walk (Intel SDM):
 *   CR3 -> PML4[47:39] -> PDPT[38:30] -> PD[29:21] -> PT[20:12] -> 4 KiB frame
 *   PD entry with PTE_PS -> 2 MiB "huge" page (what we use for the direct map).
 *
 * Editing tables: every table is a physical frame from pmm_alloc(); with the
 * bootstrap identity map, P2V(pa) reaches it until we relocate the kernel.
 */
#include <cpu.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <types.h>

#define PTE_ADDR(e)   ((uint64_t)(e) & 0x000FFFFFFFFFF000ull)
#define PTE_FLAGS(e)  ((uint32_t)(e) & 0xFFFu)
#define HUGE_2MB      0x200000ull

#define PML4_INDEX(va)  (((uint64_t)(va) >> 39) & 0x1FFu)
#define PDPT_INDEX(va)  (((uint64_t)(va) >> 30) & 0x1FFu)
#define PD_INDEX(va)    (((uint64_t)(va) >> 21) & 0x1FFu)
#define PT_INDEX(va)    (((uint64_t)(va) >> 12) & 0x1FFu)

static uint64_t *kernel_pml4;
static uint64_t *cur_pml4;
static phys_addr_t cur_pml4_phys;
static uint32_t global_bit;

static inline int is_huge_pd(uint64_t pd_entry)
{
    return (pd_entry & (PTE_PRESENT | PTE_PS)) == (PTE_PRESENT | PTE_PS);
}

static void invlpg(uintptr_t va)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static void flush_tlb(void)
{
    __asm__ volatile ("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");
}

/* Which PML4 to edit: after higher-half, kernel VAs use the upper half only. */
static uint64_t *pml4_for(uint32_t va)
{
    if (KERNEL_VBASE != 0 && (uint64_t)va >= KERNEL_VBASE)
        return kernel_pml4;
    return cur_pml4;
}

static uint64_t *table_at(phys_addr_t phys)
{
    return (uint64_t *)P2V(phys);
}

/* Ensure PML4 -> PDPT -> PD exist; return pointer to PD entry for `va`. */
static uint64_t *get_pd(uint32_t va, int create, uint32_t flags)
{
    uint64_t *pml4 = pml4_for(va);
    uint64_t *pml4e = &pml4[PML4_INDEX(va)];

    if (!(*pml4e & PTE_PRESENT)) {
        if (!create)
            return NULL;
        phys_addr_t pdpt_phys = pmm_alloc();
        if (!pdpt_phys)
            return NULL;
        memset(P2V(pdpt_phys), 0, PAGE_SIZE);
        *pml4e = pdpt_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }

    uint64_t *pdpt = table_at(PTE_ADDR(*pml4e));
    uint64_t *pdpte = &pdpt[PDPT_INDEX(va)];

    if (!(*pdpte & PTE_PRESENT)) {
        if (!create)
            return NULL;
        phys_addr_t pd_phys = pmm_alloc();
        if (!pd_phys)
            return NULL;
        memset(P2V(pd_phys), 0, PAGE_SIZE);
        *pdpte = pd_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }

    return &table_at(PTE_ADDR(*pdpte))[PD_INDEX(va)];
}

static uint64_t *get_pte(uint32_t va, int create, uint32_t flags)
{
    uint64_t *pde = get_pd(va, create, flags);
    if (!pde)
        return NULL;
    if (is_huge_pd(*pde))
        return NULL;

    if (!(*pde & PTE_PRESENT)) {
        if (!create)
            return NULL;
        phys_addr_t pt_phys = pmm_alloc();
        if (!pt_phys)
            return NULL;
        memset(P2V(pt_phys), 0, PAGE_SIZE);
        *pde = pt_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    }

    uint64_t *pt = table_at(PTE_ADDR(*pde));
    return &pt[PT_INDEX(va)];
}

/* Identity VA==PA: kmalloc lives in [KHEAP_START,KHEAP_END); it grows with
   vmm_map (4 KiB). A 2 MiB huge mapping there would block that. */
static int direct_map_uses_4k(uint64_t pa)
{
    uint64_t end = pa + HUGE_2MB;
    if (pa == 0)
        return 1;
    return pa < (uint64_t)KHEAP_END && end > (uint64_t)KHEAP_START;
}

/* Identity 4 KiB map, but leave [KHEAP_START,KHEAP_END) unmapped for kmalloc. */
static void map_identity_4k_range(uint64_t start, uint64_t end)
{
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (page >= (uint64_t)KHEAP_START && page < (uint64_t)KHEAP_END)
            continue;
        vmm_map((uint32_t)page, (uint32_t)page, PTE_WRITABLE);
    }
}

static int map_2mb(uint32_t va, uint64_t pa, uint32_t flags)
{
    if (((uint64_t)va | pa) & (HUGE_2MB - 1))
        return -1;
    uint64_t *pde = get_pd(va, 1, flags);
    if (!pde || (*pde & PTE_PRESENT))
        return -1;
    if (va >= (uint32_t)KERNEL_VBASE || KERNEL_VBASE == 0)
        flags |= global_bit;
    *pde = (pa & 0x000FFFFFFFFFF000ull) | PTE_PRESENT | PTE_WRITABLE
         | PTE_PS | (flags & 0xFFF);
    invlpg(va);
    return 0;
}

void vmm_init(void)
{
    phys_addr_t pml4_phys = pmm_alloc();
    if (!pml4_phys) {
        kprintf("vmm: out of memory for PML4\n");
        return;
    }
    kernel_pml4 = table_at(pml4_phys);
    cur_pml4 = kernel_pml4;
    cur_pml4_phys = pml4_phys;
    memset(kernel_pml4, 0, PAGE_SIZE);

    uint64_t ram_end = pmm_end();
    if (ram_end > DIRECTMAP_LIMIT)
        ram_end = DIRECTMAP_LIMIT;

    /* Identity direct map: VA == PA for [0, ram_end), mostly 2 MiB pages.
       The first 2 MiB uses 4 KiB pages: vmm_unmap(0) must drop only the NULL
       page, not a 2 MiB huge mapping that covers the kernel at 1 MiB. */
    for (uint64_t pa = 0; pa < ram_end; pa += HUGE_2MB) {
        if (direct_map_uses_4k(pa)) {
            uint64_t end = pa + HUGE_2MB;
            if (end > ram_end)
                end = ram_end;
            map_identity_4k_range(pa, end);
            continue;
        }
        if (pa + HUGE_2MB <= ram_end) {
            if (map_2mb((uint32_t)pa, pa, PTE_WRITABLE) < 0)
                kprintf("vmm: direct map 2 MiB @ %llx failed\n", (unsigned long long)pa);
        } else {
            /* Tail smaller than 2 MiB: 4 KiB pages. */
            map_identity_4k_range(pa, ram_end);
        }
    }

    /* NULL must not be mapped (bootinfo lives at 0x1000, still mapped). */
    vmm_unmap(0);

    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0 | (1ull << 16)) : "memory"); /* CR0.WP */

    global_bit = 0;
    if (cpu_has(CPU_PGE)) {
        global_bit = PTE_GLOBAL;
        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= 1 << 7;
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }

    /* Pre-create 4 KiB tables for kmalloc heap (same idea as i386). */
    for (uint32_t va = (uint32_t)KHEAP_START; va < (uint32_t)KHEAP_END; va += (uint32_t)HUGE_2MB) {
        uint64_t *pde = get_pd(va, 1, PTE_WRITABLE);
        if (!pde || (*pde & PTE_PRESENT))
            continue;
        phys_addr_t pt = pmm_alloc();
        if (!pt)
            break;
        memset(P2V(pt), 0, PAGE_SIZE);
        *pde = pt | PTE_PRESENT | PTE_WRITABLE | global_bit;
    }
}

int vmm_map(uint32_t va, uint32_t pa, uint32_t flags)
{
    uint64_t *pte = get_pte(va, 1, flags);
    if (!pte || (*pte & PTE_PRESENT))
        return -1;
    if (KERNEL_VBASE == 0 || va >= (uint32_t)KERNEL_VBASE)
        flags |= global_bit;
    *pte = ((uint64_t)pa & 0x000FFFFFFFFFF000ull) | (flags & 0xFFF) | PTE_PRESENT;
    invlpg(va);
    return 0;
}

int vmm_map_big(uint32_t va, uint64_t pa, uint32_t flags)
{
    if (va < (uint32_t)KERNEL_VBASE && KERNEL_VBASE != 0)
        return -1;
    return map_2mb(va, pa, flags);
}

uint32_t vmm_unmap(uint32_t va)
{
    uint64_t *pde = get_pd(va, 0, 0);
    if (!pde)
        return 0;
    if (is_huge_pd(*pde)) {
        if (PML4_INDEX(va) != PML4_INDEX((uint32_t)(va + 1)))
            return 0;
        uint32_t pa = (uint32_t)PTE_ADDR(*pde);
        *pde = 0;
        invlpg(va);
        return pa;
    }
    uint64_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;
    uint32_t pa = (uint32_t)PTE_ADDR(*pte);
    *pte = 0;
    invlpg(va);
    return pa;
}

uint32_t vmm_translate(uint32_t va)
{
    uint64_t *pde = get_pd(va, 0, 0);
    if (!pde || !(*pde & PTE_PRESENT))
        return 0;
    if (is_huge_pd(*pde))
        return (uint32_t)(PTE_ADDR(*pde) | (va & (HUGE_2MB - 1)));

    uint64_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;
    return (uint32_t)(PTE_ADDR(*pte) | (va & 0xFFF));
}

uint32_t vmm_get_flags(uint32_t va)
{
    uint64_t *pte = get_pte(va, 0, 0);
    if (!pte)
        return 0;
    if (is_huge_pd(*pte))
        return PTE_FLAGS(*pte);
    return (*pte & PTE_PRESENT) ? PTE_FLAGS(*pte) : 0;
}

int vmm_protect(uint32_t va, uint32_t flags)
{
    uint64_t *pte = get_pte(va, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;
    *pte = PTE_ADDR(*pte) | (flags & 0xFFF) | PTE_PRESENT;
    invlpg(va);
    return 0;
}

void vmm_unmap_range(uint32_t start, uint32_t end)
{
    for (uint32_t va = start & ~0xFFFu; va < end; va += PAGE_SIZE) {
        uint64_t *pte = get_pte(va, 0, 0);
        if (!pte || !(*pte & PTE_PRESENT))
            continue;
        if (!(*pte & PTE_SHARED))
            pmm_free((uint32_t)PTE_ADDR(*pte));
        *pte = 0;
        invlpg(va);
    }
}

/*
 * Address spaces (phase E): with KERNEL_VBASE==0 we clone the whole PML4 so
 * mm_create() gets a distinct CR3. Leaf tables are still shared — REVIEW before fork.
 */
uint32_t vmm_pd_create(void)
{
    phys_addr_t phys = pmm_alloc();
    if (!phys)
        return 0;
    uint64_t *child = table_at(phys);
    memcpy(child, cur_pml4, PAGE_SIZE);
    return (uint32_t)phys;
}

uint32_t vmm_pd_current(void)
{
    return (uint32_t)cur_pml4_phys;
}

void vmm_pd_switch(uint32_t phys)
{
    if ((phys_addr_t)phys == cur_pml4_phys)
        return;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((phys_addr_t)phys) : "memory");
    cur_pml4 = table_at(phys);
    cur_pml4_phys = phys;
}

void vmm_pd_destroy(uint32_t phys)
{
    if ((phys_addr_t)phys == cur_pml4_phys)
        return;
    /* REVIEW: walk and free user leaf tables only. */
    pmm_free(phys);
}

uint32_t vmm_pd_fork(void)
{
    /* REVIEW: copy-on-write like arch/i386/paging.c once user AS is split. */
    return vmm_pd_create();
}

int vmm_cow_fault(uint32_t va)
{
    (void)va;
    return -1;
}
