/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * memlayout.h — the x86_64 virtual memory map
 *
 * A 64-bit address has 48 usable bits; the top 16 must copy bit 47
 * ("canonical"), so the space is two halves with a huge hole between them.
 * Like Linux, programs get the bottom half and the kernel the top half:
 *
 *   0x0000000000000000 - 0x00007FFFFFFFEFFF  user space (128 TiB), per process.
 *                        A fixed-address program (busybox: 0x400000) can be
 *                        loaded where it was linked. (include/user.h)
 *   0xFFFF800000000000 - +(end of RAM)       "direct map": all physical RAM at
 *                        +DIRECTMAP_BASE, 2 MiB pages. P2V/V2P. (PML4 entry 256:
 *                        512 GiB of it)
 *   0xFFFFFFFF80000000 - ...                 the kernel image, linked here
 *                        (-2 GiB: `-mcmodel=kernel` lets code address it with
 *                        32-bit sign-extended constants, so no 64-bit
 *                        immediates). Physical 0 maps here. (PML4 entry 511)
 *   0xFFFFFFFFBE000000 - 0xFFFFFFFFBFFFFFFF  kernel stacks, one window per
 *                        thread slot with an unmapped guard page under each
 *                        stack (kernel/thread.c, "kernel stacks")
 *   0xFFFFFFFFC0000000 - 0xFFFFFFFFC3FFFFFF  kernel heap (kmalloc big blocks)
 *   0xFFFFFFFFC4000000 - 0xFFFFFFFFC7FFFFFF  device registers, uncached
 *   0xFFFFFFFFC8000000 - top                 framebuffers, write-combined
 *
 * The same layout as i386 (heap, MMIO, framebuffers at 0xF0/0xF4/0xF8...),
 * one level up. The kernel half lives in just two PML4 entries (256 and
 * 511). They exist from boot, and every process's PML4 points at the same
 * tables, so every kernel mapping made later is seen by all processes
 * without copying anything.
 */
#ifndef ARCH_X86_64_MEMLAYOUT_H
#define ARCH_X86_64_MEMLAYOUT_H

#define PAGE_SIZE        4096   /* an int, as on i386: ~(PAGE_SIZE - 1) is then a full-width mask */
#define KERNEL_LOAD      0x100000ull            /* physical: where UEFI puts us */
#define DIRECTMAP_BASE   0xFFFF800000000000ull  /* virtual address of physical 0 */
#define KERNEL_VBASE     0xFFFFFFFF80000000ull  /* the image: physical 0 is here too */
#define KERNEL_SPACE_START DIRECTMAP_BASE       /* everything above is the kernel's */

/* The low zone (pmm.h): the kernel's own RAM, below 2 GiB, as 32-bit
   frame addresses that every device can reach. The UEFI loader puts the
   initrd below it (KERNEL_RAM_LIMIT). RAM above it is the high zone, for
   programs' pages only (HIGHMEM); the direct map covers both. */
#define DIRECTMAP_LIMIT  0x80000000ull
#define HIGHMEM          1

#define KSTACK_START     0xFFFFFFFFBE000000ull  /* thread.c: guard page + stack per slot */
#define KSTACK_END       0xFFFFFFFFC0000000ull
#define KHEAP_START      0xFFFFFFFFC0000000ull
#define KHEAP_END        0xFFFFFFFFC4000000ull
#define MMIO_BASE        0xFFFFFFFFC4000000ull  /* e1000 first, then pci_map_mmio */
#define MMIO_END         0xFFFFFFFFC8000000ull
#define WC_BASE          0xFFFFFFFFC8000000ull  /* ioremap_wc: to the end of memory */

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <types.h>

/* Physical <-> virtual. A physical address is reached through the direct
   map; V2P also accepts an address in the kernel image (a static variable),
   which is the other alias of the same RAM. One compare, no table walk. */
#define P2V(pa) ((void *)((uintptr_t)(phys_addr_t)(pa) + DIRECTMAP_BASE))
#define V2P(va) ((phys_addr_t)((uintptr_t)(va) >= KERNEL_VBASE \
                               ? (uintptr_t)(va) - KERNEL_VBASE \
                               : (uintptr_t)(va) - DIRECTMAP_BASE))
#endif

#endif
