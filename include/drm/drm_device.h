/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DRM_DEVICE_H
#define DRM_DEVICE_H

#include <drm/drm_mode_config.h>
#include <stdint.h>

/*
 * One display device (Linux: include/drm/drm_device.h). A driver embeds it
 * in its own struct and gets it from devm_drm_dev_alloc.
 */

struct drm_driver;
struct drm_simple_display_pipe;
struct pci_dev;

struct drm_device {
    const struct drm_driver *driver;
    struct pci_dev *pdev;
    void *dev_private;
    struct drm_mode_config mode_config;
    int registered;

    /* MyOS: the one display pipe (drm_simple_display_pipe_init sets it). */
    struct drm_simple_display_pipe *pipe;
    /* MyOS: video memory, from drmm_vram_helper_init (Linux keeps this in
       the VRAM memory manager, dev->vram_mm). */
    uint32_t vram_base;                 /* physical */
    uint32_t vram_size;                 /* bytes */
    volatile void *vram;                /* kernel address, write-combined */
    /* MyOS: 1 while /dev/dri/card0 sets a mode: keep video memory's
       contents (its buffers live there) instead of clearing it. */
    int keep_vram;
};

#endif
