/*
 * memlayout.h — x86_64 virtual memory (phase B bootstrap)
 *
 * UEFI leaves us in long mode with an identity map. Until arch/x86_64/paging.c
 * installs a higher half, the kernel runs at its load address (1 MiB) and
 * P2V(pa) == (void *)(uintptr_t)pa.
 */
#ifndef ARCH_X86_64_MEMLAYOUT_H
#define ARCH_X86_64_MEMLAYOUT_H

#include <stdint.h>
#include <types.h>

#define PAGE_SIZE        4096u
#define KERNEL_LOAD      0x100000ull
#define KERNEL_VBASE     0ull

/* Direct-mapped physical RAM for pmm/paging (raise as the port grows). */
#define DIRECTMAP_LIMIT  0x80000000ull   /* 2 GiB for now */
#define KHEAP_START      0x02000000ull   /* 32 MiB: kmalloc below initrd */
#define KHEAP_END        0x04000000ull

#ifndef __ASSEMBLER__
#define P2V(pa) ((void *)((uintptr_t)(phys_addr_t)(pa)))
#define V2P(va) ((phys_addr_t)(uintptr_t)(va))
#endif

#endif
