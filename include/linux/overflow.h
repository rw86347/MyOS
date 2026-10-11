/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_OVERFLOW_H
#define LINUX_OVERFLOW_H
/*
 * Sizes that can't wrap around. A size computed from a count a device or
 * a user gave us could overflow and come out small; then the allocation
 * succeeds and the copy after it runs off the end. These helpers saturate
 * at SIZE_MAX instead, so the allocation just fails.
 */
#include <linux/compiler_types.h>
#include <stddef.h>
#include <stdint.h>

#define check_add_overflow(a, b, d)  __builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d)  __builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d)  __builtin_mul_overflow(a, b, d)

static inline size_t size_mul(size_t a, size_t b)
{
    size_t r;
    return __builtin_mul_overflow(a, b, &r) ? SIZE_MAX : r;
}
static inline size_t size_add(size_t a, size_t b)
{
    size_t r;
    return __builtin_add_overflow(a, b, &r) ? SIZE_MAX : r;
}
#define array_size(n, size)                size_mul((n), (size))
#define array3_size(a, b, c)               size_mul(size_mul((a), (b)), (c))
/* A struct ending in a flexible array of n elements. */
#define flex_array_size(p, m, n)           size_mul((n), sizeof(*(p)->m))
#define struct_size(p, m, n)               size_add(sizeof(*(p)), flex_array_size(p, m, n))
#define struct_size_t(T, m, n)             struct_size((T *)NULL, m, n)
/* An on-stack, zeroed T with room for count elements in its flexible
   array member, and `name` pointing at it. */
#define DEFINE_RAW_FLEX(T, name, member, count)                                 \
    union {                                                                     \
        u8 bytes[sizeof(T) + sizeof(((T *)0)->member[0]) * (count)];            \
        T obj;                                                                  \
    } name##_u = { };                                                           \
    T *name = &name##_u.obj
#define __struct_size(p)                   __builtin_object_size(p, 0)
#endif
