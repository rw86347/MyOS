/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_THERMAL_H
#define LINUX_THERMAL_H
/* No thermal framework (CONFIG_THERMAL off): the driver's thermal zone and
   cooling device code is compiled out by its own #ifdefs. */
#include <linux/types.h>
struct thermal_zone_device;
struct thermal_cooling_device;
#define THERMAL_TEMP_INVALID (-274000)
#endif
