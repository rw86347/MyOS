/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_RFKILL_H
#define LINUX_RFKILL_H
/* Why a radio is switched off by hardware (the laptop's Wi-Fi switch,
   or another owner). Numbers as in Linux's rfkill ABI. */
#include <stdbool.h>

enum rfkill_hard_block_reasons {
    RFKILL_HARD_BLOCK_SIGNAL    = 1 << 0,
    RFKILL_HARD_BLOCK_NOT_OWNER = 1 << 1,
};
struct rfkill;
/* The radio's software switch ("airplane mode"): the 802.11 core owns it. */
bool rfkill_soft_blocked(struct rfkill *rfkill);
bool rfkill_blocked(struct rfkill *rfkill);
#endif
