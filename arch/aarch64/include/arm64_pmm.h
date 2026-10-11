/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_ARM64_PMM_H
#define ARCH_AARCH64_ARM64_PMM_H

#include <stdint.h>

void     arm64_pmm_init(uintptr_t dtb);
uint32_t arm64_pmm_alloc(void); /* physical address of a 4 KiB frame, or 0 */
void     arm64_pmm_free(uint32_t phys);
uint32_t arm64_pmm_free_frames(void);
uint32_t arm64_pmm_total_frames(void);

#endif
