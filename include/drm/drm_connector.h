#ifndef DRM_CONNECTOR_H
#define DRM_CONNECTOR_H

#include <drm/drm_modes.h>
#include <stdint.h>

/*
 * A connector: where a screen plugs in (VGA, DVI, a laptop panel, or a
 * virtual one for a VM). It knows which modes the screen can show
 * (Linux: include/drm/drm_connector.h).
 *
 * Linux keeps the probed modes in a list of allocated modes. Ours is a
 * fixed table: no allocation, and a display has a few dozen modes at most.
 */

#define DRM_MODE_CONNECTOR_VGA     1
#define DRM_MODE_CONNECTOR_LVDS    7      /* a laptop panel */
#define DRM_MODE_CONNECTOR_VIRTUAL 15

#define DRM_CONNECTOR_MAX_MODES 24

struct drm_device;
struct drm_connector;

struct drm_connector_funcs {
    /* Fill the mode list, at most max_width x max_height. Drivers use
       drm_helper_probe_single_connector_modes here, as in Linux. */
    int (*fill_modes)(struct drm_connector *connector, uint32_t max_width,
                      uint32_t max_height);
};

struct drm_connector_helper_funcs {
    /* Add the screen's modes (from its EDID, or a fixed list) with
       drm_mode_probed_add. Returns how many. */
    int (*get_modes)(struct drm_connector *connector);
    /* Optional: can this connector carry this mode? */
    enum drm_mode_status (*mode_valid)(struct drm_connector *connector,
                                       const struct drm_display_mode *mode);
};

struct drm_connector {
    struct drm_device *dev;
    int connector_type;
    const struct drm_connector_funcs *funcs;
    const struct drm_connector_helper_funcs *helper_private;
    struct drm_display_mode modes[DRM_CONNECTOR_MAX_MODES];
    int num_modes;
};

int drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                       const struct drm_connector_funcs *funcs, int connector_type);

static inline void drm_connector_helper_add(struct drm_connector *connector,
                                            const struct drm_connector_helper_funcs *funcs)
{
    connector->helper_private = funcs;
}

/* The standard fill_modes: empty the list, ask get_modes, drop what doesn't
   fit max_width x max_height or fails mode_valid. Returns the count. */
int drm_helper_probe_single_connector_modes(struct drm_connector *connector,
                                            uint32_t max_width, uint32_t max_height);

#endif
