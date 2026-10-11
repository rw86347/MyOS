/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * bochs.c — the Bochs / QEMU standard VGA card (PCI 1234:1111), step 11b
 *
 * The display card of QEMU's "-vga std", of UTM's "VGA" display, and of the
 * Bochs emulator. It is a VGA card plus the "Bochs VBE extensions" (DISPI):
 * a few 16-bit registers that set any resolution and color depth directly —
 * no video BIOS calls, no timing registers. Its video memory (VRAM, 16 MiB
 * by default) is a linear framebuffer at PCI BAR0: pixel (x, y) is at
 * BAR0 + y * pitch + x * 4.
 *
 * Written from QEMU's docs/specs/standard-vga.rst and the Bochs VBE
 * register list — not from Linux's bochs.c (GPLv2). The structure is the
 * usual Linux DRM one (see drm_drv.h): probe, a connector with its modes,
 * a simple display pipe with mode_valid / enable / disable / update.
 *
 * Registers: write the register number to port 0x1CE, then read or write
 * its value at port 0x1CF. Port I/O is slow, but we touch them only on a
 * mode change and on a pan (two writes per console scroll).
 */
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <io.h>
#include <pci.h>
#include "../drm_internal.h"

#define DISPI_INDEX 0x1CE
#define DISPI_DATA  0x1CF

enum {
    DISPI_ID = 0,               /* 0xB0C0 + version */
    DISPI_XRES = 1,
    DISPI_YRES = 2,
    DISPI_BPP = 3,
    DISPI_ENABLE = 4,
    DISPI_BANK = 5,             /* 64 KiB window at 0xA0000; unused with the LFB */
    DISPI_VIRT_WIDTH = 6,       /* pixels per line in memory (the pitch) */
    DISPI_VIRT_HEIGHT = 7,      /* lines in memory (QEMU works it out itself) */
    DISPI_X_OFFSET = 8,         /* the top-left pixel shown */
    DISPI_Y_OFFSET = 9,
    DISPI_VIDEO_MEMORY_64K = 10,/* VRAM size in 64 KiB units (version 0xB0C5) */
};

#define DISPI_ID0         0xB0C0
#define DISPI_ID5         0xB0C5
#define DISPI_ENABLED     0x01
#define DISPI_LFB_ENABLED 0x40

/* The VGA attribute controller: bit 5 of its index register ("palette
   address source") switches the picture on. Reading the input status
   register first resets the controller's index/data toggle. */
#define VGA_ATT_W     0x3C0
#define VGA_IS1_RC    0x3DA
#define VGA_AR_ENABLE 0x20

struct bochs_device {
    struct drm_device dev;
    struct drm_connector connector;
    struct drm_simple_display_pipe pipe;
    uint16_t version;
    uint16_t x_offset, y_offset;        /* last written, to skip repeats */
};

static void dispi_write(uint16_t reg, uint16_t val)
{
    outw(DISPI_INDEX, reg);
    outw(DISPI_DATA, val);
}

static uint16_t dispi_read(uint16_t reg)
{
    outw(DISPI_INDEX, reg);
    return inw(DISPI_DATA);
}

static struct bochs_device *to_bochs(struct drm_simple_display_pipe *pipe)
{
    return (struct bochs_device *)((char *)pipe - offsetof(struct bochs_device, pipe));
}

/* --- the connector: a virtual screen that shows any standard mode ------- */

static int bochs_connector_get_modes(struct drm_connector *connector)
{
    /* QEMU can describe its screen with EDID too; until we read that, the
       standard modes up to 2560x1600. 1920x1200: the largest 2008 laptop
       panel (WUXGA) — and it fits in the default 16 MiB of VRAM. */
    int n = drm_add_modes_noedid(connector, 2560, 1600);
    drm_set_preferred_mode(connector, 1920, 1200);
    return n;
}

static const struct drm_connector_helper_funcs bochs_connector_helper_funcs = {
    .get_modes = bochs_connector_get_modes,
};

static const struct drm_connector_funcs bochs_connector_funcs = {
    .fill_modes = drm_helper_probe_single_connector_modes,
};

/* --- the display pipe --------------------------------------------------- */

static enum drm_mode_status bochs_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
                                                  const struct drm_display_mode *mode)
{
    struct bochs_device *b = to_bochs(pipe);
    if (mode->hdisplay % 8)
        return MODE_BAD_WIDTH;          /* the card's line length unit */
    if ((uint32_t)mode->hdisplay * mode->vdisplay * 4 > b->dev.vram_size)
        return MODE_MEM;
    return MODE_OK;
}

static void bochs_unblank(int on)
{
    (void)inb(VGA_IS1_RC);
    outb(VGA_ATT_W, on ? VGA_AR_ENABLE : 0);
}

/* Show the plane's framebuffer from its source position on. The card has
   no "start address"; it has a pixel offset (x, y) into a virtual screen
   VIRT_WIDTH pixels wide. So the framebuffer's place in VRAM becomes
   lines and pixels, plus the plane's own source position. */
static void bochs_set_offset(struct bochs_device *b, struct drm_plane_state *ps)
{
    struct drm_framebuffer *fb = ps->fb;
    uint32_t pitch = fb->pitches[0];
    uint16_t x = (uint16_t)((fb->offsets[0] % pitch) / 4 + (ps->src_x >> 16));
    uint16_t y = (uint16_t)(fb->offsets[0] / pitch + (ps->src_y >> 16));
    if (x != b->x_offset) {
        dispi_write(DISPI_X_OFFSET, x);
        b->x_offset = x;
    }
    if (y != b->y_offset) {
        dispi_write(DISPI_Y_OFFSET, y);
        b->y_offset = y;
    }
}

static void bochs_pipe_enable(struct drm_simple_display_pipe *pipe,
                              struct drm_crtc_state *crtc_state,
                              struct drm_plane_state *plane_state)
{
    struct bochs_device *b = to_bochs(pipe);
    const struct drm_display_mode *m = &crtc_state->mode;
    struct drm_framebuffer *fb = plane_state->fb;

    /* Off while changing, then the new mode, then on with the linear
       framebuffer. (Enabling also clears VRAM: QEMU does that for us.) */
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_XRES, m->hdisplay);
    dispi_write(DISPI_YRES, m->vdisplay);
    dispi_write(DISPI_BANK, 0);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)(fb->pitches[0] / 4));
    dispi_write(DISPI_VIRT_HEIGHT, (uint16_t)fb->height);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    b->x_offset = b->y_offset = 0;
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);
    bochs_unblank(1);
    bochs_set_offset(b, plane_state);
}

static void bochs_pipe_disable(struct drm_simple_display_pipe *pipe)
{
    (void)pipe;
    bochs_unblank(0);
}

static void bochs_pipe_update(struct drm_simple_display_pipe *pipe,
                              struct drm_plane_state *old_state)
{
    (void)old_state;
    bochs_set_offset(to_bochs(pipe), pipe->plane.state);
}

static const struct drm_simple_display_pipe_funcs bochs_pipe_funcs = {
    .mode_valid = bochs_pipe_mode_valid,
    .enable = bochs_pipe_enable,
    .disable = bochs_pipe_disable,
    .update = bochs_pipe_update,
};

static const uint32_t bochs_formats[] = { DRM_FORMAT_XRGB8888 };

/* --- probe ------------------------------------------------------------------ */

static const struct drm_driver bochs_driver = {
    .driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    .name = "bochs",
    .desc = "Bochs / QEMU standard VGA",
    .major = 1,
    .minor = 0,
};

/* The size of a memory BAR: write all ones, read back which address bits
   the card lets us set (the size is a power of two), restore. */
static uint32_t bar_size(struct pci_dev *pdev, int bar)
{
    uint8_t reg = (uint8_t)(0x10 + bar * 4);
    uint32_t old = pci_read_config(pdev, reg);
    pci_write_config(pdev, reg, 0xFFFFFFFFu);
    uint32_t mask = pci_read_config(pdev, reg) & ~0xFu;
    pci_write_config(pdev, reg, old);
    return mask ? ~mask + 1 : 0;
}

static int bochs_probe(struct pci_dev *pdev)
{
    uint16_t id = dispi_read(DISPI_ID);
    if (id < DISPI_ID0 || id > DISPI_ID5) {
        kprintf("drm: bochs: no VBE extensions (id %04x)\n", id);
        return -1;
    }

    struct bochs_device *b = devm_drm_dev_alloc(pdev, &bochs_driver, struct bochs_device, dev);
    if (!b)
        return -1;
    b->version = id;

    uint32_t fb_base = pdev->bar[0] & ~0xFu;
    uint32_t fb_size = bar_size(pdev, 0);
    if (id >= DISPI_ID5) {
        uint32_t vram = (uint32_t)dispi_read(DISPI_VIDEO_MEMORY_64K) * 64 * 1024;
        if (vram && (!fb_size || vram < fb_size))
            fb_size = vram;
    }
    if (!fb_base || !fb_size || drmm_vram_helper_init(&b->dev, fb_base, fb_size) < 0) {
        drm_err(&b->dev, "can't map video memory\n");
        return -1;
    }

    drmm_mode_config_init(&b->dev);
    b->dev.mode_config.max_width = 8192;
    b->dev.mode_config.max_height = 8192;

    drm_connector_init(&b->dev, &b->connector, &bochs_connector_funcs,
                       DRM_MODE_CONNECTOR_VIRTUAL);
    drm_connector_helper_add(&b->connector, &bochs_connector_helper_funcs);
    if (drm_simple_display_pipe_init(&b->dev, &b->pipe, &bochs_pipe_funcs,
                                     bochs_formats, 1, NULL, &b->connector) < 0)
        return -1;
    b->connector.funcs->fill_modes(&b->connector, 8192, 8192);

    drm_dev_register(&b->dev, 0);
    drm_fbdev_generic_setup(&b->dev, 32);
    return 0;
}

/* Linux matches drivers to cards with a table and the PCI core calls
   probe; with one display driver we look for the card ourselves. */
void bochs_init(void)
{
    struct pci_dev *pdev = pci_find(0x1234, 0x1111);
    if (pdev)
        bochs_probe(pdev);
}
