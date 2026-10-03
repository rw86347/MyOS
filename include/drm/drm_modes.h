#ifndef DRM_MODES_H
#define DRM_MODES_H

#include <stdint.h>

/*
 * A display mode: the picture size and the timings of the video signal that
 * carries it (Linux: include/drm/drm_modes.h, same field names). A real
 * screen needs the timings; the Bochs/QEMU card only uses the size.
 *
 *   |<- hdisplay ->|<- front porch ->|<- sync ->|<- back porch ->|
 *   0         hdisplay         hsync_start   hsync_end        htotal
 *
 * clock is the pixel clock in kHz: clock * 1000 / (htotal * vtotal) is the
 * refresh rate.
 */

#define DRM_DISPLAY_MODE_LEN 32

#define DRM_MODE_TYPE_PREFERRED (1 << 3)
#define DRM_MODE_TYPE_DRIVER    (1 << 6)

struct drm_display_mode {
    int clock;                          /* kHz */
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t flags;
    uint32_t type;                      /* DRM_MODE_TYPE_* */
    char name[DRM_DISPLAY_MODE_LEN];    /* "1920x1200" */
};

/* Why a mode can't be used. Values as in Linux. */
enum drm_mode_status {
    MODE_OK = 0,
    MODE_H_ILLEGAL = 3,                 /* width not possible */
    MODE_V_ILLEGAL = 4,                 /* height not possible */
    MODE_BAD_WIDTH = 5,                 /* e.g. not a multiple of 8 */
    MODE_MEM = 10,                      /* not enough video memory */
    MODE_BAD = -2,
    MODE_ERROR = -3,
};

struct drm_connector;

/* Add a mode to the connector's list (Linux takes a kmalloc'd mode; ours is
   copied into a fixed table, so a stack copy is fine). */
void drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode);

/* Mark the mode of this size PREFERRED (the one used at boot). */
void drm_set_preferred_mode(struct drm_connector *connector, int hpref, int vpref);

/* "1920x1200" into mode->name. */
void drm_mode_set_name(struct drm_display_mode *mode);

/* Refresh rate in Hz, rounded. */
int drm_mode_vrefresh(const struct drm_display_mode *mode);

#endif
