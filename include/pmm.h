#ifndef PMM_H
#define PMM_H

#include <stdint.h>

/* Physical memory manager: 4 KiB frames of RAM, by physical address. */

void     pmm_init(void);             /* read the BIOS memory map */
uint32_t pmm_alloc(void);            /* a free frame's physical address, or 0 */
void     pmm_free(uint32_t phys);
uint32_t pmm_free_count(void);       /* frames currently free */
uint32_t pmm_usable_count(void);     /* frames of usable RAM in total */
uint32_t pmm_end(void);              /* physical end of usable RAM (bytes) */

#endif
