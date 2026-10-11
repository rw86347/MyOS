/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_TYPES_H
#define LINUX_TYPES_H

/*
 * The Linux driver API ("the shim", docs/WIFI-PLAN.md): our headers with
 * Linux's names, so a Linux driver compiles against MyOS unchanged. The
 * kernel underneath is ours; only the names and the behaviour a driver
 * relies on are Linux's.
 *
 * types.h: the fixed-width names, and the "endian" ones that say how a
 * device stores a number (__le32: little-endian, as on x86, so a plain
 * u32; __be32: big-endian, network order).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/stddef.h>
#include <types.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;   /* (long long on every CPU, as in Linux: drivers print it with %llu) */
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef long long s64;
typedef u8  __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s8  __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;

typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;
typedef u16 __sum16;
typedef u32 __wsum;

typedef long ssize_t;
typedef u64 dma_addr_t;             /* a device's view of memory: 64 bits always */
typedef phys_addr_t resource_size_t;
typedef unsigned int gfp_t;
typedef unsigned int fmode_t;
typedef long long loff_t;
typedef s64 ktime_t;
typedef s64 time64_t;            /* seconds since 1970 */

#ifndef __cplusplus
typedef unsigned long ulong;
#endif

#endif
