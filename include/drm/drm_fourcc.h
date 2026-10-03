#ifndef DRM_FOURCC_H
#define DRM_FOURCC_H

#include <stdint.h>

/*
 * Pixel formats, named by four characters (Linux: include/uapi/drm/drm_fourcc.h).
 * XRGB8888: 32 bits per pixel, in memory blue, green, red, unused — one
 * little-endian 32-bit word 0x00RRGGBB. The only format MyOS uses so far.
 */
#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                                 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_RGB565   fourcc_code('R', 'G', '1', '6')

#define DRM_FORMAT_MOD_LINEAR    0ull
#define DRM_FORMAT_MOD_INVALID   0x00ffffffffffffffull

struct drm_format_info {
    uint32_t format;
    uint8_t depth;                      /* color bits */
    uint8_t num_planes;
    uint8_t cpp[4];                     /* bytes per pixel, per plane */
};

/* The description of a format, or NULL if we don't know it. */
const struct drm_format_info *drm_format_info(uint32_t format);

#endif
