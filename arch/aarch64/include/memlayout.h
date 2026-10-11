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

/* Identity-mapped bring-up: physical == kernel direct pointer. */
#define P2V(pa) ((void *)(uintptr_t)(pa))

#endif
