/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_UNALIGNED_H
#define LINUX_UNALIGNED_H

/* Numbers that may sit at any address (in a frame, after a 1-byte field).
   x86 loads them fine; memcpy says so to the compiler without a fault on
   CPUs that care. */
#include <linux/byteorder.h>
#include <string.h>

#define __get_unaligned_t(t, p) ({ t _v; memcpy(&_v, (p), sizeof _v); _v; })
#define __put_unaligned_t(t, v, p) do { t _v = (v); memcpy((p), &_v, sizeof _v); } while (0)
#define get_unaligned(p)        __get_unaligned_t(__typeof__(*(p)), p)
#define put_unaligned(v, p)     __put_unaligned_t(__typeof__(*(p)), v, p)
static inline u16 get_unaligned_le16(const void *p) { return le16_to_cpu(__get_unaligned_t(u16, p)); }
static inline u32 get_unaligned_le32(const void *p) { return le32_to_cpu(__get_unaligned_t(u32, p)); }
static inline u64 get_unaligned_le64(const void *p) { return le64_to_cpu(__get_unaligned_t(u64, p)); }
static inline u16 get_unaligned_be16(const void *p) { return be16_to_cpu(__get_unaligned_t(u16, p)); }
static inline u32 get_unaligned_be32(const void *p) { return be32_to_cpu(__get_unaligned_t(u32, p)); }
static inline void put_unaligned_le16(u16 v, void *p) { __put_unaligned_t(u16, cpu_to_le16(v), p); }
static inline void put_unaligned_le32(u32 v, void *p) { __put_unaligned_t(u32, cpu_to_le32(v), p); }
static inline void put_unaligned_le64(u64 v, void *p) { __put_unaligned_t(u64, cpu_to_le64(v), p); }
static inline void put_unaligned_be16(u16 v, void *p) { __put_unaligned_t(u16, cpu_to_be16(v), p); }
static inline void put_unaligned_be32(u32 v, void *p) { __put_unaligned_t(u32, cpu_to_be32(v), p); }

#endif
