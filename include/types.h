/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * types.h — widths that must not follow the C pointer
 *
 * A 32-bit pointer cannot name GOP at 0x400000000 or RAM above 4 GiB.
 * Physical addresses are therefore 64-bit on every chip. Virtual
 * addresses stay native (uintptr_t). arch/x86_64 will keep this file;
 * it must not grow an #ifdef per call site.
 */
#ifndef MYOS_TYPES_H
#define MYOS_TYPES_H

#include <stdint.h>

typedef uint64_t phys_addr_t;

#endif
