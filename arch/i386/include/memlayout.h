/*
 * memlayout.h — the i386 virtual memory map
 *
 * With paging on, every address the CPU uses is VIRTUAL and is translated
 * through page tables to a PHYSICAL RAM address. We use the same split as
 * Linux on i386 ("3G/1G"), because Linux programs expect it:
 *
 *   0x00000000 - 0xBFFFFFFF   user space (3 GiB): a program's code, data,
 *                             heap, stack. Different for every process.
 *                             Page 0 is never mapped, so NULL derefs fault.
 *   0xC0000000 - 0xEFFFFFFF   "direct map": physical RAM 0..768 MiB mapped
 *                             1:1 at +0xC0000000. The kernel itself lives
 *                             here (physical 0x100000 -> 0xC0100000),
 *                             mapped with 4 MiB pages.
 *   0xF0000000 - 0xFEFFFFFF   kernel heap (kmalloc), mapped page by page
 *   0xFF000000 - 0xFFFFFFFF   unused for now
 *
 * The kernel half is identical in every process, so a system call or
 * interrupt can run kernel code without switching page tables.
 *
 * RAM above 768 MiB ("highmem" in Linux) is ignored for now — plenty for
 * 20-year-old machines and QEMU.
 */
#ifndef ARCH_I386_MEMLAYOUT_H
#define ARCH_I386_MEMLAYOUT_H

#define PAGE_SIZE        4096
#define KERNEL_VBASE     0xC0000000u    /* virtual address of physical 0 */
#define DIRECTMAP_LIMIT  0x30000000u    /* 768 MiB of physical RAM direct-mapped */
#define KHEAP_START      0xF0000000u
#define KHEAP_END        0xFF000000u

#ifndef __ASSEMBLER__
#include <stdint.h>

/* Convert between physical addresses and their direct-map virtual address.
   (Only valid for physical addresses below DIRECTMAP_LIMIT.) */
#define P2V(pa) ((void *)((uintptr_t)(pa) + KERNEL_VBASE))
#define V2P(va) ((uintptr_t)(va) - KERNEL_VBASE)
#endif

#endif
