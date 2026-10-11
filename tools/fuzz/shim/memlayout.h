/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* memlayout.h for the host fuzz build.
 *
 * The kernel maps physical address 0 at 0xC0000000, so P2V just adds
 * KERNEL_VBASE. That cannot be imitated literally here, for a reason that is
 * really about pointer width: pmm_alloc returns a uint32_t, and a malloc'd
 * pointer on a 64-bit host does not fit in one.
 *
 * So a "physical address" in the fuzz build is an offset into one arena, and
 * P2V adds the arena's base. That keeps pmm_alloc's type honest, and it buys
 * the thing this target exists for: the arena is carved into pages separated
 * by redzones that AddressSanitizer is told to treat as poisoned, so a write
 * one byte past a 4096-byte block is reported with a stack trace instead of
 * quietly landing in the next frame the way it does in QEMU. Four of the
 * ext2 bugs found on 2026-10-03 were exactly that write.
 */
#ifndef FUZZ_SHIM_MEMLAYOUT_H
#define FUZZ_SHIM_MEMLAYOUT_H

#include <stdint.h>

#define PAGE_SIZE        4096
#define KERNEL_VBASE     0u             /* unused: see fuzz_arena below */
#define DIRECTMAP_LIMIT  0x30000000u
#define KHEAP_START      0u
#define KHEAP_END        0u

/* The base of the arena that physical addresses are offsets into. Defined in
   tools/fuzz/diskstubs.c. Offset 0 is never handed out, so that pmm_alloc can
   keep using 0 to mean "out of memory". */
extern uint8_t *fuzz_arena;

#define P2V(pa) ((void *)(fuzz_arena + (uintptr_t)(uint32_t)(pa)))
#define V2P(va) ((uint32_t)((uint8_t *)(va) - fuzz_arena))

#endif
