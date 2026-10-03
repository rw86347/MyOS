#ifndef ARCH_I386_PAGING_H
#define ARCH_I386_PAGING_H

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004
#define PTE_PCD      0x010      /* cache disabled: device registers */
#define PTE_PS       0x080      /* in a directory entry: a 4 MiB page (PSE) */
#define PTE_GLOBAL   0x100      /* kernel pages: keep in the TLB across CR3 loads */
#define PTE_SHARED   0x200      /* OS bit: frame is shared, don't free it on unmap */
#define PTE_COW      0x400      /* OS bit: copy-on-write (read-only until written) */

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

/* The flags of the page at va (0 if not mapped). */
uint32_t vmm_get_flags(uint32_t va);

/* Change the flags of a mapped page (e.g. make it read-only). */
int vmm_protect(uint32_t va, uint32_t flags);

/* Unmap [start, end) in the current address space, dropping one owner of
   each page (PTE_SHARED pages are left alone). For munmap and brk. */
void vmm_unmap_range(uint32_t start, uint32_t end);

/* --- address spaces (page directories), by physical address ----------- */
uint32_t vmm_pd_create(void);       /* empty user half, kernel half shared */
uint32_t vmm_pd_current(void);
void     vmm_pd_switch(uint32_t pd);    /* load CR3 (no-op if already there) */
void     vmm_pd_destroy(uint32_t pd);   /* free its user pages; not current! */

/* Copy-on-write copy of the current address space's user half. */
uint32_t vmm_pd_fork(void);

/* A write hit a copy-on-write page: give the writer its own copy (or the
   page itself if nobody else shares it). 0, or -1 if va isn't COW. */
int vmm_cow_fault(uint32_t va);

#endif
