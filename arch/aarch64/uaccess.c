/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * uaccess.c — copy to/from EL0 pointers (identity map bring-up)
 *
 * Range-check only for now; demand paging and fixups come with per-process maps.
 */
#include "uaccess.h"

#include <string.h>

static int user_range_ok(const void *user, size_t n)
{
    uintptr_t start = (uintptr_t)user;
    uintptr_t end;

    if (n == 0)
        return 0;
    if (start < ARM64_USER_MIN)
        return -1;
    if (start + n < start)
        return -1;
    end = start + n;
    if (end > ARM64_USER_MAX)
        return -1;
    return 0;
}

int copy_from_user(void *dst, const void *user_src, size_t n)
{
    if (user_range_ok(user_src, n) < 0)
        return -1;
    memcpy(dst, user_src, n);
    return 0;
}

int copy_to_user(void *user_dst, const void *src, size_t n)
{
    if (user_range_ok(user_dst, n) < 0)
        return -1;
    memcpy(user_dst, src, n);
    return 0;
}
