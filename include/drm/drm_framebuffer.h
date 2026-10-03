#ifndef DRM_FRAMEBUFFER_H
#define DRM_FRAMEBUFFER_H

#include <drm/drm_fourcc.h>
#include <stdint.h>

struct drm_device;

/*
 * A framebuffer: a picture in memory the card can show (Linux:
 * include/drm/drm_framebuffer.h). It may be taller than the screen; the
 * plane picks which part is shown.
 *
 * Linux keeps the memory in a GEM buffer object (fb->obj[0]) and a driver
 * adds that object's place in video memory to offsets[0]. MyOS has one
 * buffer, the start of video memory, so offsets[0] alone is the place.
 */
struct drm_framebuffer {
    struct drm_device *dev;
    const struct drm_format_info *format;
    uint32_t width, height;             /* pixels */
    uint32_t pitches[4];                /* bytes per line, per plane */
    uint32_t offsets[4];                /* bytes from the start of video memory */
};

#endif
