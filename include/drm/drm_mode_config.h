#ifndef DRM_MODE_CONFIG_H
#define DRM_MODE_CONFIG_H

#include <stdint.h>

/*
 * What the display hardware as a whole can do (Linux:
 * include/drm/drm_mode_config.h): the smallest and largest picture, the
 * default color depth.
 */

struct drm_device;

struct drm_mode_config_funcs {
    int unused;                         /* Linux: fb_create, atomic_check, ... */
};

struct drm_mode_config {
    int min_width, min_height;
    int max_width, max_height;
    uint32_t preferred_depth;           /* 24 for XRGB8888 */
    const struct drm_mode_config_funcs *funcs;
};

/* Linux's "managed" init: freed with the device. Ours is never freed. */
int drmm_mode_config_init(struct drm_device *dev);

#endif
