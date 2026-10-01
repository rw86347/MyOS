#ifndef ARCH_I386_PAGING_H
#define ARCH_I386_PAGING_H

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004
#define PTE_PS       0x080      /* in a directory entry: a 4 MiB page (PSE) */
#define PTE_GLOBAL   0x100      /* kernel pages: keep in the TLB across CR3 loads */
#define PTE_SHARED   0x200      /* OS bit: frame is shared, don't free it on unmap */

/* Map all RAM into the kernel's direct map and remove the low identity map.
   Needs pmm_init() first. */
void vmm_init(void);

/* Map one 4 KiB page: virtual `va` -> physical `pa`. Returns 0, or -1 if
   out of memory or `va` is already mapped. */
int vmm_map(uint32_t va, uint32_t pa, uint32_t flags);

/* Unmap one page. Returns the physical address it pointed to (0 if none). */
uint32_t vmm_unmap(uint32_t va);

/* The physical address `va` maps to, or 0 if not mapped. */
uint32_t vmm_translate(uint32_t va);

/* Change the flags of a mapped page (e.g. make it read-only). */
int vmm_protect(uint32_t va, uint32_t flags);

/* Unmap every user page (below 0xC0000000), free the frames (except
   PTE_SHARED ones) and the page tables. Ends a program's use of memory. */
void vmm_unmap_user_all(void);

#endif
