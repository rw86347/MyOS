#ifndef DRM_FBDEV_GENERIC_H
#define DRM_FBDEV_GENERIC_H

struct drm_device;

/*
 * fbdev emulation (Linux: include/drm/drm_fbdev_generic.h). Sets the
 * preferred mode, puts a framebuffer in video memory, and offers it as
 * /dev/fb0 — the older, simpler interface programs like the X server's
 * fbdev driver use — and to the console (fbcon). preferred_bpp: 32.
 */
void drm_fbdev_generic_setup(struct drm_device *dev, unsigned int preferred_bpp);

#endif
