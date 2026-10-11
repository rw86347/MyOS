/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef USER_TEST_H
#define USER_TEST_H

/* Run the embedded user programs (ring 3, int 0x80) and check the results. */
void user_selftest(void);

#endif
