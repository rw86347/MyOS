/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_SORT_H
#define LINUX_SORT_H
#include <linux/types.h>
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
          void (*swap)(void *, void *, int));
#endif
