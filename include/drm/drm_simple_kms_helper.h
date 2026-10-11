/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DRM_SIMPLE_KMS_HELPER_H
#define DRM_SIMPLE_KMS_HELPER_H

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <stdint.h>

/*
 * The "simple display pipe" (Linux: include/drm/drm_simple_kms_helper.h):
 * one CRTC, one plane, one encoder, one connector — what a simple card or a
 * firmware framebuffer has. The driver fills in four callbacks; the DRM
 * core keeps the states and calls them.
 */

struct drm_simple_display_pipe;

struct drm_simple_display_pipe_funcs {
    /* Can the hardware show this mode at all? (Video memory, limits.) */
    enum drm_mode_status (*mode_valid)(struct drm_simple_display_pipe *pipe,
                                       const struct drm_display_mode *mode);
    /* Switch the display on in crtc_state->mode, showing plane_state->fb.
       MyOS rule: the visible part of the framebuffer is black afterwards
       (the console relies on it to skip drawing empty cells — most of a
       fresh screen). Bochs clears video memory when it enables a mode. */
    void (*enable)(struct drm_simple_display_pipe *pipe,
                   struct drm_crtc_state *crtc_state,
                   struct drm_plane_state *plane_state);
    /* Switch it off (blank). */
    void (*disable)(struct drm_simple_display_pipe *pipe);
    /* The plane changed (another framebuffer, or panned): pipe->plane.state
       is the new state, old_plane_state the one before. */
    void (*update)(struct drm_simple_display_pipe *pipe,
                   struct drm_plane_state *old_plane_state);
};

struct drm_simple_display_pipe {
    struct drm_crtc crtc;
    struct drm_plane plane;
    struct drm_encoder encoder;
    struct drm_connector *connector;
    const struct drm_simple_display_pipe_funcs *funcs;
    /* MyOS: the states live here (Linux allocates them per commit). */
    struct drm_crtc_state crtc_state;
    struct drm_plane_state plane_state;
};

int drm_simple_display_pipe_init(struct drm_device *dev,
                                 struct drm_simple_display_pipe *pipe,
                                 const struct drm_simple_display_pipe_funcs *funcs,
                                 const uint32_t *formats, unsigned int format_count,
                                 const uint64_t *format_modifiers,
                                 struct drm_connector *connector);

#endif
