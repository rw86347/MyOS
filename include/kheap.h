#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>
#include <stdint.h>

/* Kernel heap. Needs pmm_init() and vmm_init() first. Memory is 16-byte
   aligned and NOT zeroed. kmalloc returns NULL when out of memory. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Bytes in use, bytes free inside the heap, bytes of address space mapped. */
void  kheap_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *mapped);

#endif
