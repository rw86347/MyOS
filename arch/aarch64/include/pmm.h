/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_PMM_H
#define ARCH_AARCH64_PMM_H

#include "arm64_pmm.h"

static inline void pmm_init(void) { /* arm64_pmm_init() from boot */ }

static inline uint32_t pmm_alloc(void)
{
    return arm64_pmm_alloc();
}

static inline void pmm_free(uint32_t phys)
{
    arm64_pmm_free(phys);
}

#endif
