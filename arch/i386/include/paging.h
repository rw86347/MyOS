#ifndef ARCH_I386_PAGING_H
#define ARCH_I386_PAGING_H

#include <stdint.h>

/* Page table entry flags */
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004
#define PTE_PS       0x080      /* in a directory entry: a 4 MiB page (PSE) */

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

/* May a user program access [va, va+len)? True only if every page is mapped
   with the user bit (and writable, if `write`). Check every pointer a
   syscall gets before touching it. */
int vmm_user_ok(uint32_t va, uint32_t len, int write);

/* Unmap every user page (below 0xC0000000), free the frames and the page
   tables. Ends a program's use of memory. */
void vmm_unmap_user_all(void);

#endif
