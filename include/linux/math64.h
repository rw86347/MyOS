/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_MATH64_H
#define LINUX_MATH64_H
#include <linux/kernel.h>
static inline u64 div_u64_rem(u64 n, u32 d, u32 *rem) { *rem = (u32)(n % d); return n / d; }
static inline s64 div_s64_rem(s64 n, s32 d, s32 *rem) { *rem = (s32)(n % d); return n / d; }
static inline u64 div64_u64_rem(u64 n, u64 d, u64 *rem) { *rem = n % d; return n / d; }
static inline s64 div64_s64(s64 n, s64 d) { return n / d; }
#define div64_ul(n, d)        ((n) / (d))
#define DIV64_U64_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(n, d) (((u64)(n) + (d) - 1) / (d))
#define DIV_U64_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
static inline u64 mul_u32_u32(u32 a, u32 b) { return (u64)a * b; }
static inline u64 mul_u64_u32_div(u64 a, u32 m, u32 d) { return mul_u64_u64_div_u64(a, m, d); }
#endif
