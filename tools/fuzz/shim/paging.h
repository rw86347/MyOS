/* paging.h for the host fuzz build. The only thing the filesystem code wants
   from it is vmm_translate, which the block layer uses to turn a kernel
   virtual address into a physical one for DMA. The stub is the identity,
   matching the shim memlayout.h. */
#ifndef FUZZ_SHIM_PAGING_H
#define FUZZ_SHIM_PAGING_H

#include <stdint.h>

uint32_t vmm_translate(uint32_t va);

#endif
