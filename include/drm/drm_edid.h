#ifndef DRM_EDID_H
#define DRM_EDID_H

struct drm_connector;

/*
 * EDID is the description a screen sends of itself (its modes, its name).
 * Reading it comes with real hardware; until then a connector without one
 * gets the standard modes (Linux: include/drm/drm_edid.h).
 */

/* Add the standard (VESA DMT) modes up to hdisplay x vdisplay. Returns how
   many were added. */
int drm_add_modes_noedid(struct drm_connector *connector, int hdisplay, int vdisplay);

#endif
