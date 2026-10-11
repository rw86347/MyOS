/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_BITFIELD_H
#define LINUX_BITFIELD_H

/* Fields inside a register value, named by their mask: FIELD_GET(0x0f00, v)
   is bits 8-11 of v, shifted down. The shift is the mask's lowest set bit. */
#include <linux/bitops.h>
#include <linux/byteorder.h>

#define __bf_shf(m)          __builtin_ctzll((u64)(m))
#define FIELD_GET(m, v)      ((__typeof__(m))(((v) & (m)) >> __bf_shf(m)))
#define FIELD_PREP(m, v)     ((((__typeof__(m))(v)) << __bf_shf(m)) & (m))
#define FIELD_FIT(m, v)      (!((((__typeof__(m))(v)) << __bf_shf(m)) & ~(m)))
#define FIELD_PREP_CONST(m, v) FIELD_PREP(m, v)   /* (constant-folds: usable in initializers) */
#define FIELD_MAX(m)         ((__typeof__(m))((m) >> __bf_shf(m)))
#define FIELD_MODIFY(m, p, v) (*(p) = (*(p) & ~(m)) | FIELD_PREP(m, v))

#define u8_get_bits(v, m)    ((u8)FIELD_GET(m, v))
#define u16_get_bits(v, m)   ((u16)FIELD_GET(m, v))
#define u32_get_bits(v, m)   ((u32)FIELD_GET(m, v))
#define u64_get_bits(v, m)   ((u64)FIELD_GET(m, v))
#define le16_get_bits(v, m)  ((u16)FIELD_GET(m, le16_to_cpu(v)))
#define le32_get_bits(v, m)  ((u32)FIELD_GET(m, le32_to_cpu(v)))
#define le64_get_bits(v, m)  ((u64)FIELD_GET(m, le64_to_cpu(v)))
#define u8_encode_bits(v, m)  ((u8)FIELD_PREP(m, v))
#define u16_encode_bits(v, m) ((u16)FIELD_PREP(m, v))
#define u32_encode_bits(v, m) ((u32)FIELD_PREP(m, v))
#define u64_encode_bits(v, m) ((u64)FIELD_PREP(m, v))
#define le16_encode_bits(v, m) cpu_to_le16(u16_encode_bits(v, m))
#define le32_encode_bits(v, m) cpu_to_le32(u32_encode_bits(v, m))
#define le64_encode_bits(v, m) cpu_to_le64(u64_encode_bits(v, m))
#define u32_replace_bits(old, v, m) (((old) & ~(m)) | u32_encode_bits(v, m))
#define u16_replace_bits(old, v, m) (((old) & ~(m)) | u16_encode_bits(v, m))
#define u8_replace_bits(old, v, m)  (((old) & ~(m)) | u8_encode_bits(v, m))
#define le16_replace_bits(old, v, m) (((old) & ~cpu_to_le16(m)) | le16_encode_bits(v, m))
#define le32_replace_bits(old, v, m) (((old) & ~cpu_to_le32(m)) | le32_encode_bits(v, m))

#endif
