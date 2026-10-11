/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>
#include <stdint.h>

/* Kernel heap. Needs pmm_init() and vmm_init() first. Memory is 16-byte
   aligned and NOT zeroed. kmalloc returns NULL when out of memory. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* A block whose pages are physically contiguous (for DMA): a run of
   pages through the direct map. kfree frees it like any other. */
void *kmalloc_contig(size_t size);

/* The usable size of an allocation (its size class, or its block): at
   least what was asked for. */
size_t kheap_size(const void *ptr);

/* Pages the heap has taken from the PMM so far (it never gives any back). */
uint32_t kheap_pages(void);

/* Bytes in use, bytes free inside the heap, bytes of address space mapped. */
void  kheap_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *mapped);

#endif
