/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_UACCESS_H
#define ARCH_AARCH64_UACCESS_H

#include <stddef.h>
#include <stdint.h>

#include "memlayout.h"

int arm64_user_range_ok(uintptr_t start, size_t n);
int copy_from_user(void *dst, const void *user_src, size_t n);
int copy_to_user(void *user_dst, const void *src, size_t n);

#endif
