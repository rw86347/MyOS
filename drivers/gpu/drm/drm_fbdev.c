/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * drm_fbdev.c — fbdev emulation: a DRM display as /dev/fb0 (step 11b)
 *
 * DRM is the modern interface; /dev/fb0 is the old, simple one that many
 * programs (and the console) still use. Linux bridges them with "fbdev
 * emulation" (drivers/gpu/drm/drm_fbdev_generic.c and drm_fb_helper.c),
 * and so do we: one framebuffer at the start of video memory, the
 * connector's preferred mode at boot, and fb_ops that turn fbdev requests
 * (set a mode, pan, blank) into DRM mode sets.
 *
 * The framebuffer is as tall as video memory allows (yres_virtual), so the
 * console and programs can pan through it — double buffering for free.
 */
#include <drm/drm_connector.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <errno.h>
#include <linux/fb.h>
#include <pci.h>
#include <string.h>
#include "drm_internal.h"

static struct drm_fbdev {
    struct drm_device *dev;
    struct drm_framebuffer fb;
    struct fb_info info;
} fbdev;

/* The connector's mode of this size, or NULL. */
static const struct drm_display_mode *find_mode(struct drm_device *dev, uint32_t w, uint32_t h)
{
    struct drm_connector *c = dev->pipe->connector;
    for (int i = 0; i < c->num_modes; i++)
        if (c->modes[i].hdisplay == w && c->modes[i].vdisplay == h)
            return &c->modes[i];
    return NULL;
}

static const struct drm_display_mode *preferred_mode(struct drm_device *dev)
{
    struct drm_connector *c = dev->pipe->connector;
    for (int i = 0; i < c->num_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
            return &c->modes[i];
    return c->num_modes ? &c->modes[0] : NULL;
}

/* Lines of video memory at this width (the most yres_virtual can be). */
static uint32_t max_lines(struct drm_device *dev, uint32_t width)
{
    uint32_t lines = dev->vram_size / (width * 4);
    return lines > 0xFFFF ? 0xFFFF : lines;     /* the card's registers are 16-bit */
}

/* var's resolution, timings and pixel format from mode. */
static void var_from_mode(struct fb_var_screeninfo *var, const struct drm_display_mode *m)
{
    var->xres = var->xres_virtual = m->hdisplay;
    var->yres = m->vdisplay;
    var->bits_per_pixel = 32;
    var->grayscale = 0;
    var->red = (struct fb_bitfield){ 16, 8, 0 };
    var->green = (struct fb_bitfield){ 8, 8, 0 };
    var->blue = (struct fb_bitfield){ 0, 8, 0 };
    var->transp = (struct fb_bitfield){ 0, 0, 0 };
    var->nonstd = 0;
    var->activate = FB_ACTIVATE_NOW;
    var->height = var->width = 0;               /* size in mm: unknown */
    var->accel_flags = 0;
    var->pixclock = m->clock ? 1000000000u / (uint32_t)m->clock : 0;   /* ps */
    var->left_margin = m->htotal - m->hsync_end;
    var->right_margin = m->hsync_start - m->hdisplay;
    var->hsync_len = m->hsync_end - m->hsync_start;
    var->upper_margin = m->vtotal - m->vsync_end;
    var->lower_margin = m->vsync_start - m->vdisplay;
    var->vsync_len = m->vsync_end - m->vsync_start;
    var->sync = 0;
    var->vmode = FB_VMODE_NONINTERLACED;
    var->rotate = 0;
    var->colorspace = 0;
}

/* --- fb_ops ------------------------------------------------------------------ */

static int fbdev_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct drm_device *dev = ((struct drm_fbdev *)info->par)->dev;
    if (var->bits_per_pixel && var->bits_per_pixel != 32)
        return -EINVAL;                         /* XRGB8888 only, for now */
    const struct drm_display_mode *m = find_mode(dev, var->xres, var->yres);
    if (!m || drm_mode_check(dev, m) != MODE_OK)
        return -EINVAL;
    if (var->xres_virtual > var->xres)
        return -EINVAL;                         /* no horizontal panning */
    uint32_t want = var->yres_virtual, most = max_lines(dev, m->hdisplay);
    uint32_t xoff = var->xoffset, yoff = var->yoffset, activate = var->activate;
    var_from_mode(var, m);
    var->activate = activate;
    /* Linux's rule: 0 or too few means "just the screen"; asking for more
       than there is fails. */
    var->yres_virtual = want < m->vdisplay ? m->vdisplay : want;
    if (var->yres_virtual > most)
        return -EINVAL;
    var->xoffset = xoff;
    var->yoffset = yoff;
    if (xoff || yoff > var->yres_virtual - var->yres)
        return -EINVAL;
    return 0;
}

static int fbdev_set_par(struct fb_info *info)
{
    struct drm_fbdev *f = info->par;
    const struct drm_display_mode *m = find_mode(f->dev, info->var.xres, info->var.yres);
    if (!m)
        return -EINVAL;
    f->fb.width = m->hdisplay;
    f->fb.height = info->var.yres_virtual;
    f->fb.pitches[0] = m->hdisplay * 4;
    f->fb.offsets[0] = 0;
    int err = drm_mode_commit(f->dev, m, &f->fb);
    if (err)
        return err;
    info->fix.line_length = f->fb.pitches[0];
    if (info->var.yoffset)
        drm_plane_pan(f->dev, info->var.yoffset);
    return 0;
}

static int fbdev_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
    drm_plane_pan(((struct drm_fbdev *)info->par)->dev, var->yoffset);
    return 0;
}

static int fbdev_blank(int blank, struct fb_info *info)
{
    drm_blank(((struct drm_fbdev *)info->par)->dev, blank != FB_BLANK_UNBLANK);
    return 0;
}

void drm_fbdev_restore(void)
{
    if (fbdev.dev)
        fbdev_set_par(&fbdev.info);
}

static const struct fb_ops fbdev_ops = {
    .fb_check_var = fbdev_check_var,
    .fb_set_par = fbdev_set_par,
    .fb_pan_display = fbdev_pan_display,
    .fb_blank = fbdev_blank,
};

/* --- setup ------------------------------------------------------------------------ */

void drm_fbdev_generic_setup(struct drm_device *dev, unsigned int preferred_bpp)
{
    if (preferred_bpp != 32 || fbdev.dev)
        return;
    const struct drm_display_mode *m = preferred_mode(dev);
    if (!m) {
        drm_err(dev, "no usable display mode\n");
        return;
    }

    struct fb_info *info = &fbdev.info;
    fbdev.dev = dev;
    fbdev.fb.dev = dev;
    fbdev.fb.format = drm_format_info(DRM_FORMAT_XRGB8888);
    info->par = &fbdev;
    info->fbops = &fbdev_ops;
    info->screen_base = dev->vram;
    info->screen_size = dev->vram_size;

    struct fb_fix_screeninfo *fix = &info->fix;
    memset(fix, 0, sizeof *fix);
    size_t n = strlen(dev->driver->name);       /* "bochsdrmfb", as in Linux */
    if (n > sizeof fix->id - 6)
        n = sizeof fix->id - 6;
    memcpy(fix->id, dev->driver->name, n);
    memcpy(fix->id + n, "drmfb", 5);
    fix->smem_start = dev->vram_base;
    fix->smem_len = dev->vram_size;
    fix->type = FB_TYPE_PACKED_PIXELS;
    fix->visual = FB_VISUAL_TRUECOLOR;
    fix->ypanstep = 1;
    fix->accel = FB_ACCEL_NONE;

    memset(&info->var, 0, sizeof info->var);
    var_from_mode(&info->var, m);
    info->var.yres_virtual = max_lines(dev, m->hdisplay);
    if (fbdev_set_par(info) < 0) {
        drm_err(dev, "can't set %s\n", m->name);
        fbdev.dev = NULL;
        return;
    }

    struct pci_dev *pdev = dev->pdev;
    klog("drm: %s %04x:%04x, %lu MiB, %s\n", dev->driver->name,
            pdev ? pdev->vendor : 0, pdev ? pdev->device : 0,
            dev->vram_size >> 20, m->name);
    register_framebuffer(info);
}
