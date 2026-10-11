/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * drm_test.c — boot-time self-tests for the display stack (only in `make test`)
 *
 * The DRM core's helpers on their own (mode names, refresh rates, the
 * standard mode list), then — if QEMU's std VGA was found — what the
 * Bochs driver and fbdev made of it: the mode list, the video memory
 * limit, the write-combined mapping, and the console size.
 */
#include <cpu.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/fb.h>
#include <paging.h>
#include <selftest.h>
#include <string.h>
#include <vt.h>
#include "../drivers/gpu/drm/drm_internal.h"

static void test_helpers(void)
{
    static struct drm_connector c;      /* 24 modes: too big for the stack */
    drm_connector_init(NULL, &c, NULL, DRM_MODE_CONNECTOR_VGA);
    check(drm_add_modes_noedid(&c, 1024, 768) == 3,
          "drm: standard modes up to 1024x768 are 640x480, 800x600, 1024x768");
    check(strcmp(c.modes[2].name, "1024x768") == 0, "drm: mode names");
    check(drm_mode_vrefresh(&c.modes[0]) == 60 && drm_mode_vrefresh(&c.modes[2]) == 60,
          "drm: refresh rates from the timings (60 Hz)");
    drm_set_preferred_mode(&c, 800, 600);
    check((c.modes[1].type & DRM_MODE_TYPE_PREFERRED) && !(c.modes[0].type & DRM_MODE_TYPE_PREFERRED),
          "drm: preferred mode marked");
    const struct drm_format_info *f = drm_format_info(DRM_FORMAT_XRGB8888);
    check(f && f->cpp[0] == 4 && f->depth == 24, "drm: XRGB8888 is 4 bytes per pixel");
    check(DRM_FORMAT_XRGB8888 == 0x34325258, "drm: fourcc matches Linux");
}

static void test_bochs(void)
{
    struct drm_device *dev = drm_primary;
    struct drm_connector *c = dev->pipe->connector;
    const struct drm_display_mode *pref = NULL, *big = NULL;
    for (int i = 0; i < c->num_modes; i++) {
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
            pref = &c->modes[i];
        if (c->modes[i].hdisplay == 2560)
            big = &c->modes[i];
    }
    check(pref && pref->hdisplay == 1920 && pref->vdisplay == 1200,
          "bochs: 1920x1200 is the preferred mode");
    check(big != NULL, "bochs: 2560x1600 fits in 16 MiB of video memory");

    struct drm_display_mode m = *pref;
    m.hdisplay = 4096;
    m.vdisplay = 4096;
    check(drm_mode_check(dev, &m) == MODE_MEM, "bochs: a mode bigger than video memory refused");
    m.hdisplay = 1001;
    m.vdisplay = 700;
    check(drm_mode_check(dev, &m) == MODE_BAD_WIDTH, "bochs: width not a multiple of 8 refused");

    check(vmm_translate((uint32_t)dev->vram) == dev->vram_base &&
          vmm_translate((uint32_t)dev->vram + dev->vram_size - 4) == dev->vram_base + dev->vram_size - 4,
          "drm: video memory mapped into the kernel, all of it");
    if (cpu_has(CPU_PAT))
        check(((rdmsr(0x277) >> 8) & 0xFF) == 0x01, "pat: entry 1 is write-combining");

    struct fb_info *fb = fb_get(0);
    check(fb && fb->var.xres == 1920 && fb->fix.line_length == 7680 &&
          fb->var.yres_virtual == dev->vram_size / 7680,
          "fbdev: 1920x1200, panning through all of video memory");
    check(vt_cols() == 120 && vt_rows() == 37, "fbcon: 120x37 cells of 16x32 pixels");
}

void drm_selftest(void)
{
    test_helpers();
    if (drm_primary)
        test_bochs();
}
