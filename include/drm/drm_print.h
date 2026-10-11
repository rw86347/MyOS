/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DRM_PRINT_H
#define DRM_PRINT_H

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <kprintf.h>

/* Driver messages, prefixed with the driver's name (Linux: include/drm/drm_print.h). */
#define drm_info(drm, fmt, ...) kprintf("drm: %s: " fmt, (drm)->driver->name, ##__VA_ARGS__)
#define drm_err(drm, fmt, ...)  kprintf("drm: %s: error: " fmt, (drm)->driver->name, ##__VA_ARGS__)
#define drm_dbg(drm, fmt, ...)  klog("drm: %s: " fmt, (drm)->driver->name, ##__VA_ARGS__)

#endif
