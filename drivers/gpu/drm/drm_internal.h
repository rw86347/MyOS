/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DRM_INTERNAL_H
#define DRM_INTERNAL_H

/*
 * Inside the DRM core: what the fbdev emulation uses to drive the display.
 * In Linux this is an "atomic commit" — build a new state for the CRTC and
 * the plane, check it, swap it in. With one display pipe and states that
 * live in it, a commit is: write the new state, call the driver.
 */
#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <stdint.h>

/* The first registered DRM device, or NULL. */
extern struct drm_device *drm_primary;

/* Show fb in mode, from line 0. 0, or -EINVAL if the driver rejects the mode. */
int drm_mode_commit(struct drm_device *dev, const struct drm_display_mode *mode,
                    struct drm_framebuffer *fb);

/* Show fb from line y on (y + mode height <= fb height). O(1): a register
   write or two, no copying — this is how the console scrolls. */
void drm_plane_pan(struct drm_device *dev, uint32_t y);

/* Show fb (same mode, same pitch as now) from its offsets[0] on. O(1):
   a page flip on the Bochs card is this. */
void drm_plane_show(struct drm_device *dev, struct drm_framebuffer *fb);

/* Show fb from its offsets[0] on, in the mode already set (same pitch).
   O(1): how a page flip on the Bochs card is done. */
void drm_plane_show(struct drm_device *dev, struct drm_framebuffer *fb);

/* Blank (1) or unblank (0) the display. */
void drm_blank(struct drm_device *dev, int blank);

/* The console's mode and framebuffer again (a compositor let go). */
void drm_fbdev_restore(void);

/* /dev/dri/card0 (drm_ioctl.c): made by drm_init if there is a screen. */
void drm_uapi_init(void);

/* The console's mode again, after a compositor let go of the screen. */
void drm_fbdev_restore(void);

/* /dev/dri/card0 (drm_ioctl.c): made by drm_init if there is a screen. */
void drm_uapi_init(void);

/* The display drivers' entry points, called by drm_init. */
void bochs_init(void);
void simplefb_init(void);           /* UEFI GOP, if the loader left one */

/* Can the driver show this mode? (Checks the pipe's mode_valid.) */
enum drm_mode_status drm_mode_check(struct drm_device *dev, const struct drm_display_mode *mode);

#endif
