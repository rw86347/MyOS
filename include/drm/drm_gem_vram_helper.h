#ifndef DRM_GEM_VRAM_HELPER_H
#define DRM_GEM_VRAM_HELPER_H

#include <stdint.h>

struct drm_device;

/*
 * Video memory on the card (Linux: include/drm/drm_gem_vram_helper.h).
 * A driver with dedicated VRAM tells the core where it is; the core maps it
 * into the kernel write-combined (writes are gathered into bursts — the fast
 * way to fill a framebuffer).
 *
 * Linux manages VRAM as buffer objects that move in and out of it. MyOS so
 * far has one buffer: the framebuffer at the start of VRAM.
 */
int drmm_vram_helper_init(struct drm_device *dev, uint32_t vram_base, uint32_t vram_size);

#endif
