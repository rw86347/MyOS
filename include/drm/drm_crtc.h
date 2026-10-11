/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DRM_CRTC_H
#define DRM_CRTC_H

#include <drm/drm_modes.h>
#include <stdbool.h>

/*
 * A CRTC ("cathode ray tube controller", the old name stuck) is the part of
 * the card that reads a framebuffer and turns it into a video signal with
 * a mode's timings (Linux: include/drm/drm_crtc.h). Its state says whether
 * it runs and in which mode.
 */

struct drm_device;
struct drm_crtc;

struct drm_crtc_state {
    struct drm_crtc *crtc;
    bool enable;                        /* wanted on */
    bool active;                        /* actually running */
    bool mode_changed;
    struct drm_display_mode mode;
};

struct drm_crtc {
    struct drm_device *dev;
    struct drm_crtc_state *state;
};

#endif
