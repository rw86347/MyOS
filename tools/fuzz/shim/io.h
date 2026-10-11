/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef FUZZ_IO_H
#define FUZZ_IO_H
#include <stdint.h>

/* There are no I/O ports on the host. random.c reads port 0x80 to time an ISA
   bus cycle; here that returns a constant, so the boot entropy pool is empty
   and the generator starts from a fixed key. Deliberate: see cycles.h. */
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline void outb(uint16_t port, uint8_t v) { (void)port; (void)v; }

#endif
