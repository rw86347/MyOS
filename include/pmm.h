/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <types.h>

/* Physical memory manager: 4 KiB frames of RAM, by physical address.
 *
 * Two zones on x86_64 (one on i386):
 *   low   below DIRECTMAP_LIMIT (2 GiB): the kernel's own memory — page
 *         tables, the heap, the caches, every buffer a device reads or
 *         writes (DMA). pmm_alloc() hands these out, as 32-bit addresses.
 *   high  the rest of RAM (the MSI's 16 GiB): only programs' private
 *         pages — heap, stack, copy-on-write copies — which no device
 *         ever touches, so no driver must reach above 4 GiB. Handed out by
 *         pmm_alloc_user(), as 64-bit addresses; it falls back to low RAM
 *         when the high zone is full or absent (i386).
 * Linux started the same way (ZONE_DMA32). pmm_free, pmm_ref and
 * pmm_shared take either. */

void     pmm_init(void);             /* read the BIOS memory map */
uint32_t pmm_alloc(void);            /* a free frame's physical address, or 0 */
/* `pages` consecutive frames, lowest first, or 0. For a framebuffer
   shadow: user mmap needs one physical base. */
uint32_t pmm_alloc_run(uint32_t pages);
void     pmm_free(phys_addr_t phys); /* drop one owner; freed when none left */
void     pmm_refs_init(void);        /* after vmm_init(): sharing, the high zone */
void     pmm_ref(phys_addr_t phys);  /* one more owner (copy-on-write) */
int      pmm_shared(phys_addr_t phys);  /* more than one owner? */
phys_addr_t pmm_alloc_user(void);    /* a frame for a program's private page */
uint32_t pmm_free_count(void);       /* frames currently free (both zones) */
uint32_t pmm_usable_count(void);     /* frames of usable RAM in total */
uint32_t pmm_end(void);              /* physical end of the low zone (bytes) */
/* The high zone's RAM ranges, for the direct map (vmm_init): range i, or 0
   past the last one. */
int      pmm_high_range(int i, phys_addr_t *start, phys_addr_t *end);

/* Called by pmm_alloc when no frame is free: free up to `want` frames
   (the page cache's clean pages) and say how many. */
void     pmm_set_reclaim(uint32_t (*fn)(uint32_t want));
#ifdef PT_AUDIT
int      pmm_owners(phys_addr_t phys);  /* 0 free, -1 not ours, else owners */
#endif

#endif
