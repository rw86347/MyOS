/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DRM_PLANE_H
#define DRM_PLANE_H

#include <stdint.h>

/*
 * A plane is one picture layer the CRTC shows: which framebuffer, which part
 * of it (src_*), and where on the screen (crtc_*). Linux: include/drm/drm_plane.h.
 * The source rectangle is in 16.16 fixed point, as in Linux: src_y >> 16 is
 * the first line shown — panning is changing src_y.
 */

struct drm_device;
struct drm_crtc;
struct drm_framebuffer;
struct drm_plane;

struct drm_plane_state {
    struct drm_plane *plane;
    struct drm_crtc *crtc;
    struct drm_framebuffer *fb;
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    uint32_t src_x, src_y;              /* 16.16 */
    uint32_t src_w, src_h;              /* 16.16 */
};

struct drm_plane {
    struct drm_device *dev;
    struct drm_plane_state *state;
};

#endif
