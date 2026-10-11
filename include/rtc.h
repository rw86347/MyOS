/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* The date and time from the CMOS clock, as seconds since 1970 (UTC,
   assuming the clock is set to UTC). */
uint32_t rtc_read_epoch(void);

#endif
