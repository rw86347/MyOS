/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * drm_core.c — the DRM/KMS core: display modes, connectors, the simple
 * display pipe (step 11b)
 *
 * Linux splits display drivers in two layers, and so do we:
 *
 *   - the driver knows one card: its registers, how much video memory it
 *     has, which modes it can show. It fills in a few callbacks.
 *   - the core (this file) knows what all displays have in common: modes
 *     and their timings, connectors and their mode lists, the pipeline
 *     framebuffer -> plane -> CRTC -> encoder -> connector -> screen.
 *
 * KMS is "kernel mode setting": the kernel, not the X server, programs the
 * display mode. The function and struct names are Linux's
 * (include/drm/...), so a Linux driver's structure carries over; the
 * implementation is MyOS's own and much smaller: one display, one pipe,
 * states that live in the pipe instead of being allocated per change.
 *
 * Nothing here is on a hot path except drm_plane_pan (the console scrolls
 * with it), which is O(1).
 */
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <errno.h>
#include <kheap.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <string.h>
#include <suspend.h>
#include "drm_internal.h"

struct drm_device *drm_primary;

/* --- pixel formats ------------------------------------------------------ */

static const struct drm_format_info formats[] = {
    { DRM_FORMAT_XRGB8888, 24, 1, { 4, 0, 0, 0 } },
    { DRM_FORMAT_RGB565,   16, 1, { 2, 0, 0, 0 } },
};

const struct drm_format_info *drm_format_info(uint32_t format)
{
    for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++)
        if (formats[i].format == format)
            return &formats[i];
    return NULL;
}

/* --- modes ---------------------------------------------------------------- */

/* Unsigned decimal into s; returns the end. */
static char *put_uint(char *s, unsigned v)
{
    char tmp[10];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n)
        *s++ = tmp[--n];
    return s;
}

void drm_mode_set_name(struct drm_display_mode *mode)
{
    char *s = put_uint(mode->name, mode->hdisplay);
    *s++ = 'x';
    s = put_uint(s, mode->vdisplay);
    *s = 0;
}

int drm_mode_vrefresh(const struct drm_display_mode *mode)
{
    uint32_t pixels = (uint32_t)mode->htotal * mode->vtotal;
    if (!pixels)
        return 0;
    return (int)(((uint32_t)mode->clock * 1000u + pixels / 2) / pixels);
}

void drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode)
{
    if (connector->num_modes == DRM_CONNECTOR_MAX_MODES)
        return;
    connector->modes[connector->num_modes++] = *mode;
}

void drm_set_preferred_mode(struct drm_connector *connector, int hpref, int vpref)
{
    for (int i = 0; i < connector->num_modes; i++) {
        struct drm_display_mode *m = &connector->modes[i];
        if (m->hdisplay == hpref && m->vdisplay == vpref)
            m->type |= DRM_MODE_TYPE_PREFERRED;
    }
}

/*
 * The standard modes: VESA's Display Monitor Timings (DMT), the list every
 * monitor of the 2000s understood — 4:3, 5:4, 16:10 for the laptop panels
 * of 2008, 16:9 for later screens. Timings are the published ones (the
 * newer, wider modes with "reduced blanking", which LCDs prefer). A card
 * without real timing hardware, like QEMU's, only uses the size.
 */
#define DMT(clk, hd, hss, hse, ht, vd, vss, vse, vt) \
    { .clock = clk, .hdisplay = hd, .hsync_start = hss, .hsync_end = hse, \
      .htotal = ht, .vdisplay = vd, .vsync_start = vss, .vsync_end = vse, \
      .vtotal = vt, .type = DRM_MODE_TYPE_DRIVER }

static const struct drm_display_mode dmt_modes[] = {
    DMT( 25175,  640,  656,  752,  800,  480,  490,  492,  525),
    DMT( 40000,  800,  840,  968, 1056,  600,  601,  605,  628),
    DMT( 65000, 1024, 1048, 1184, 1344,  768,  771,  777,  806),
    DMT( 74250, 1280, 1390, 1430, 1650,  720,  725,  730,  750),
    DMT( 71000, 1280, 1328, 1360, 1440,  800,  803,  809,  823),
    DMT(108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066),
    DMT( 88750, 1440, 1488, 1520, 1600,  900,  903,  909,  926),
    DMT(162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250),
    DMT(119000, 1680, 1728, 1760, 1840, 1050, 1053, 1059, 1080),
    DMT(148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125),
    DMT(154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235),
    DMT(268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646),
};

int drm_add_modes_noedid(struct drm_connector *connector, int hdisplay, int vdisplay)
{
    int added = 0;
    for (unsigned i = 0; i < sizeof dmt_modes / sizeof dmt_modes[0]; i++) {
        struct drm_display_mode m = dmt_modes[i];
        if (m.hdisplay > hdisplay || m.vdisplay > vdisplay)
            continue;
        drm_mode_set_name(&m);
        drm_mode_probed_add(connector, &m);
        added++;
    }
    return added;
}

/* --- connectors ----------------------------------------------------------- */

int drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                       const struct drm_connector_funcs *funcs, int connector_type)
{
    connector->dev = dev;
    connector->funcs = funcs;
    connector->connector_type = connector_type;
    connector->num_modes = 0;
    return 0;
}

enum drm_mode_status drm_mode_check(struct drm_device *dev, const struct drm_display_mode *mode)
{
    struct drm_mode_config *cfg = &dev->mode_config;
    if (mode->hdisplay < cfg->min_width || mode->hdisplay > cfg->max_width)
        return MODE_H_ILLEGAL;
    if (mode->vdisplay < cfg->min_height || mode->vdisplay > cfg->max_height)
        return MODE_V_ILLEGAL;
    struct drm_simple_display_pipe *pipe = dev->pipe;
    if (pipe && pipe->funcs->mode_valid)
        return pipe->funcs->mode_valid(pipe, mode);
    return MODE_OK;
}

int drm_helper_probe_single_connector_modes(struct drm_connector *connector,
                                            uint32_t max_width, uint32_t max_height)
{
    const struct drm_connector_helper_funcs *h = connector->helper_private;
    connector->num_modes = 0;
    if (h && h->get_modes)
        h->get_modes(connector);

    /* Keep the modes that fit and that the connector and the card accept.
       Linux keeps the rejected ones too, marked with why; we drop them. */
    int kept = 0;
    for (int i = 0; i < connector->num_modes; i++) {
        struct drm_display_mode *m = &connector->modes[i];
        if ((max_width && m->hdisplay > max_width) ||
            (max_height && m->vdisplay > max_height))
            continue;
        if (h && h->mode_valid && h->mode_valid(connector, m) != MODE_OK)
            continue;
        if (drm_mode_check(connector->dev, m) != MODE_OK)
            continue;
        connector->modes[kept++] = *m;
    }
    connector->num_modes = kept;
    return kept;
}

/* --- devices -------------------------------------------------------------- */

void *__devm_drm_dev_alloc(struct pci_dev *parent, const struct drm_driver *driver,
                           size_t size, size_t offset)
{
    char *p = kmalloc(size);
    if (!p)
        return NULL;
    memset(p, 0, size);
    struct drm_device *dev = (struct drm_device *)(p + offset);
    dev->driver = driver;
    dev->pdev = parent;
    return p;
}

int drmm_mode_config_init(struct drm_device *dev)
{
    struct drm_mode_config *cfg = &dev->mode_config;
    cfg->min_width = cfg->min_height = 0;
    cfg->max_width = cfg->max_height = 8192;
    cfg->preferred_depth = 24;
    return 0;
}

int drmm_vram_helper_init(struct drm_device *dev, uint32_t vram_base, uint32_t vram_size)
{
    volatile void *va = ioremap_wc(vram_base, vram_size);
    if (!va)
        return -ENOMEM;
    dev->vram_base = vram_base;
    dev->vram_size = vram_size;
    dev->vram = va;
    return 0;
}

int drm_dev_register(struct drm_device *dev, unsigned long flags)
{
    (void)flags;
    dev->registered = 1;
    if (!drm_primary)
        drm_primary = dev;
    return 0;
}

void drm_init(void)
{
    bochs_init();
    if (!fb_get(0))
        simplefb_init();            /* real UEFI hardware: no Bochs card */
    drm_uapi_init();                /* /dev/dri/card0, for Wayland */
}

/* --- the simple display pipe --------------------------------------------- */

int drm_simple_display_pipe_init(struct drm_device *dev,
                                 struct drm_simple_display_pipe *pipe,
                                 const struct drm_simple_display_pipe_funcs *funcs,
                                 const uint32_t *formats_, unsigned int format_count,
                                 const uint64_t *format_modifiers,
                                 struct drm_connector *connector)
{
    (void)format_modifiers;
    /* We only know XRGB8888; the driver must offer it. */
    int ok = 0;
    for (unsigned i = 0; i < format_count; i++)
        if (formats_[i] == DRM_FORMAT_XRGB8888)
            ok = 1;
    if (!ok)
        return -EINVAL;

    pipe->funcs = funcs;
    pipe->connector = connector;
    pipe->crtc.dev = pipe->plane.dev = pipe->encoder.dev = dev;
    pipe->crtc.state = &pipe->crtc_state;
    pipe->plane.state = &pipe->plane_state;
    pipe->crtc_state.crtc = &pipe->crtc;
    pipe->plane_state.plane = &pipe->plane;
    pipe->plane_state.crtc = &pipe->crtc;
    pipe->encoder.encoder_type = DRM_MODE_ENCODER_NONE;
    dev->pipe = pipe;
    return 0;
}

int drm_mode_commit(struct drm_device *dev, const struct drm_display_mode *mode,
                    struct drm_framebuffer *fb)
{
    struct drm_simple_display_pipe *pipe = dev->pipe;
    if (!pipe || drm_mode_check(dev, mode) != MODE_OK ||
        fb->width < mode->hdisplay || fb->height < mode->vdisplay)
        return -EINVAL;

    struct drm_crtc_state *cs = &pipe->crtc_state;
    struct drm_plane_state *ps = &pipe->plane_state;
    if (cs->active && pipe->funcs->disable)
        pipe->funcs->disable(pipe);
    cs->mode = *mode;
    cs->enable = cs->active = cs->mode_changed = true;
    ps->fb = fb;
    ps->crtc_x = ps->crtc_y = 0;
    ps->crtc_w = mode->hdisplay;
    ps->crtc_h = mode->vdisplay;
    ps->src_x = ps->src_y = 0;
    ps->src_w = (uint32_t)mode->hdisplay << 16;
    ps->src_h = (uint32_t)mode->vdisplay << 16;
    pipe->funcs->enable(pipe, cs, ps);
    cs->mode_changed = false;
    return 0;
}

void drm_plane_pan(struct drm_device *dev, uint32_t y)
{
    struct drm_simple_display_pipe *pipe = dev->pipe;
    struct drm_plane_state old = pipe->plane_state;
    pipe->plane_state.src_y = y << 16;
    if (pipe->funcs->update)
        pipe->funcs->update(pipe, &old);
}

void drm_plane_show(struct drm_device *dev, struct drm_framebuffer *fb)
{
    struct drm_simple_display_pipe *pipe = dev->pipe;
    struct drm_plane_state old = pipe->plane_state;
    pipe->plane_state.fb = fb;
    pipe->plane_state.src_x = pipe->plane_state.src_y = 0;
    if (pipe->funcs->update)
        pipe->funcs->update(pipe, &old);
}

void drm_blank(struct drm_device *dev, int blank)
{
    struct drm_simple_display_pipe *pipe = dev->pipe;
    struct drm_crtc_state *cs = &pipe->crtc_state;
    if (blank && cs->active) {
        if (pipe->funcs->disable)
            pipe->funcs->disable(pipe);
        cs->active = false;
    } else if (!blank && !cs->active && cs->enable) {
        pipe->funcs->enable(pipe, cs, &pipe->plane_state);
        cs->active = true;
    }
}

/* Sleep (kernel/suspend.c): the main screen off, and on again only if
   sleep turned it off (one X had switched off stays off). */
void drm_suspend(int sleep)
{
    static int blanked;
    if (!drm_primary || !drm_primary->pipe)
        return;
    if (sleep) {
        blanked = drm_primary->pipe->crtc_state.active;
        drm_blank(drm_primary, 1);
    } else if (blanked) {
        drm_blank(drm_primary, 0);
        blanked = 0;
    }
}
