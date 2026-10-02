#ifndef PMM_H
#define PMM_H

#include <stdint.h>

/* Physical memory manager: 4 KiB frames of RAM, by physical address. */

void     pmm_init(void);             /* read the BIOS memory map */
uint32_t pmm_alloc(void);            /* a free frame's physical address, or 0 */
void     pmm_free(uint32_t phys);    /* drop one owner; freed when none left */
void     pmm_refs_init(void);        /* after vmm_init(): enable sharing */
void     pmm_ref(uint32_t phys);     /* one more owner (copy-on-write) */
int      pmm_shared(uint32_t phys);  /* more than one owner? */
uint32_t pmm_free_count(void);       /* frames currently free */
uint32_t pmm_usable_count(void);     /* frames of usable RAM in total */
uint32_t pmm_end(void);              /* physical end of usable RAM (bytes) */

#endif
