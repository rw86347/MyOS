/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_CACHE_H
#define LINUX_CACHE_H
/* Cache-line alignment. 64 bytes on every CPU we run on (Pentium 4's L2
   line is 128, but 64 is its L1 and what matters for false sharing).
   The _in_smp form only matters with several CPUs: nothing here. */
#include <linux/compiler_types.h>
#define L1_CACHE_BYTES               64
#define SMP_CACHE_BYTES              L1_CACHE_BYTES
#define ____cacheline_aligned        __aligned(SMP_CACHE_BYTES)
#define ____cacheline_aligned_in_smp
#define __cacheline_aligned          ____cacheline_aligned
#define __cacheline_aligned_in_smp
#endif
