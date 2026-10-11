/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef E1000_H
#define E1000_H

/* Bring up QEMU's e1000 (8086:100e) as eth0. 0, or -1 if it is not there. */
int e1000_init(void);

/* One line: IRQ count and the card's send/receive counters. For a failed ping. */
void e1000_debug(void);

#endif
