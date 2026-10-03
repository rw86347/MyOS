#ifndef DRM_ENCODER_H
#define DRM_ENCODER_H

/*
 * An encoder turns the CRTC's pixel stream into what a connector needs
 * (analog VGA, LVDS for a panel, TMDS for DVI). Linux: include/drm/drm_encoder.h.
 */

#define DRM_MODE_ENCODER_NONE    0
#define DRM_MODE_ENCODER_DAC     1
#define DRM_MODE_ENCODER_LVDS    3
#define DRM_MODE_ENCODER_VIRTUAL 5

struct drm_device;

struct drm_encoder {
    struct drm_device *dev;
    int encoder_type;
};

#endif
