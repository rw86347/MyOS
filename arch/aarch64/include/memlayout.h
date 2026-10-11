/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * memlayout.h — ARM64 virtual layout (placeholder for step 14a paging)
 *
 * Hello builds run identity-mapped at the load address from linker.ld.
 */
#ifndef ARCH_AARCH64_MEMLAYOUT_H
#define ARCH_AARCH64_MEMLAYOUT_H

#define KERNEL_LOAD_PHYS       0x40100000UL
#define PAGE_SIZE              4096u
/* Must cover QEMU virt RAM (base 0x40000000). MMU step will map this window. */
#define ARM64_DIRECTMAP_LIMIT  0x80000000ULL /* 2 GiB physical */

/*
 * Alpine busybox.static is ET_EXEC at 0x400000 (same as Linux). That VA is
 * MMIO on QEMU virt if identity-mapped, so paging aliases it onto RAM.
 */
#define ARM64_USER_IMAGE_VA    0x00400000ULL
#define ARM64_USER_IMAGE_PA    0x45000000ULL
#define ARM64_USER_IMAGE_SIZE  0x00400000ULL /* 4 MiB: binary + small brk */

/* Identity-mapped user RAM. Kernel + embedded initrd stay below this. */
#define ARM64_USER_MIN         0x40800000ULL
#define ARM64_USER_MAX         0x48000000ULL
#define ARM64_USER_STACK_TOP   0x41000000ULL
#define ARM64_STACK_BLOCK      0x40e00000ULL
#define ARM64_STACK_BLOCK_SIZE 0x00200000ULL
#define ARM64_MMAP_BASE        0x41800000ULL
#define ARM64_MMAP_END         0x42800000ULL
#define ARM64_INTERP_VA        0x43000000ULL
#define ARM64_INTERP_SIZE      0x00200000ULL
#define ARM64_RAM_BASE         0x40000000ULL

/* Identity-mapped bring-up: physical == kernel direct pointer. */
#define P2V(pa) ((void *)(uintptr_t)(pa))

#endif
