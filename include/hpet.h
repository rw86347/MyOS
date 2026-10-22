/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef HPET_H
#define HPET_H

#include <stdint.h>

/* Make the HPET drive IRQ 0 at `hz` in place of the PIT (drivers/hpet.c).
   0, or -1 if there is no usable HPET. */
int hpet_legacy_start(uint32_t hz);
void hpet_legacy_pause(int pause);      /* 1: its counter stops; 0: it runs on */

#endif
