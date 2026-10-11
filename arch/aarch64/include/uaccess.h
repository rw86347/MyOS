/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_UACCESS_H
#define ARCH_AARCH64_UACCESS_H

#include <stddef.h>
#include <stdint.h>

#define ARM64_USER_MIN  0x40200000ULL
#define ARM64_USER_MAX  0x48000000ULL /* stack pages from PMM below this for bring-up */

int copy_from_user(void *dst, const void *user_src, size_t n);
int copy_to_user(void *user_dst, const void *src, size_t n);

#endif
