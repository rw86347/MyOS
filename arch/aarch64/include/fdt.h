/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_FDT_H
#define ARCH_AARCH64_FDT_H

#include <stdint.h>

#define FDT_MAGIC 0xd00dfeedU

/* Physical address of the DTB (QEMU loads -dtb at RAM base on virt). */
int fdt_check_header(uintptr_t dtb_phys);
/* First memory bank from /memory reg (64-bit cells). Returns 0 on failure. */
int fdt_first_ram(uintptr_t dtb_phys, uint64_t *base_out, uint64_t *size_out);

#endif
