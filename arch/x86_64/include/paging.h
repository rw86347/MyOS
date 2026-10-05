#ifndef ARCH_X86_64_PAGING_H
#define ARCH_X86_64_PAGING_H

#include <stdint.h>

/*
 * x86_64 page table entry flags (bits 11:0). Same meaning as i386 for the
 * low bits; addresses live in bits 51:12 (must be canonical when used as VA).
 *
 * Virtual addresses are uintptr_t. Physical frames of RAM are 32-bit numbers
 * (pmm: below DIRECTMAP_LIMIT); vmm_map takes a 64-bit one, because device
 * memory (a 64-bit PCI BAR, a GOP framebuffer) can be above 4 GiB.
 */
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004
#define PTE_PWT      0x008
#define PTE_PCD      0x010
#define PTE_PS       0x080      /* 2 MiB at PD level, 1 GiB at PDPT (not used yet) */
#define PTE_GLOBAL   0x100
#define PTE_SHARED   0x200
#define PTE_COW      0x400
#define PTE_MAPSHARED 0x800

void vmm_init(void);

int vmm_map(uintptr_t va, uint64_t pa, uint32_t flags);
int vmm_map_big(uintptr_t va, uint64_t pa, uint32_t flags);
uint32_t pte_write_combining(void);
uint32_t vmm_unmap(uintptr_t va);
uint32_t vmm_translate(uintptr_t va);
uint32_t vmm_get_flags(uintptr_t va);
int vmm_protect(uintptr_t va, uint32_t flags);
void vmm_unmap_range(uintptr_t start, uintptr_t end);

uint32_t vmm_pd_create(void);
uint32_t vmm_pd_current(void);
void     vmm_pd_switch(uint32_t pd);
void     vmm_pd_destroy(uint32_t pd);
uint32_t vmm_pd_fork(void);
int      vmm_cow_fault(uintptr_t va);

#endif
