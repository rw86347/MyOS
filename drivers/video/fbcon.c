/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fbcon.c — the console on a graphics framebuffer (step 11b)
 *
 * In a graphics mode the card shows pixels, not characters, so the console
 * draws each character itself: the shape comes from the video BIOS's 8x16
 * font (stage2.asm copies it for us), scaled up 2x on big screens so the
 * text stays readable — 1920x1200 gives 16x32-pixel cells, 120x37 of them.
 * Like Linux's fbcon, this is a backend of the console (vt.c), which keeps
 * the text in its grid; we only ever WRITE video memory, never read it back
 * (reads from a framebuffer are uncached and very slow).
 *
 * Scrolling by panning: copying the whole screen up by one text line would
 * mean writing 9 MiB at 1920x1200. Instead the screen's start line in video
 * memory moves down by one text line (one register write to the card), and
 * only the new bottom line is drawn. Video memory is taller than the
 * screen (16 MiB = 2184 lines of 1920 pixels), so this works ~30 times;
 * then we start again at line 0 and draw the whole screen once. Linux
 * calls this SCROLL_PAN_REDRAW.
 *
 * The cursor is drawn by us too (an underline, like the VGA text cursor's
 * scan lines 14-15), and taken away by drawing the cell again.
 */
#include <bootinfo.h>
#include <kprintf.h>
#include <linux/fb.h>
#include <memlayout.h>
#include <vt.h>

#define MAX_SCALE 4

static struct fb_info *info;
static const uint8_t *font;             /* 256 glyphs x 16 rows, bit 7 = left */
static int scale, cw, ch;               /* font scale; cell width, height in pixels */
static int cols, rows;
static uint32_t pitch;                  /* pixels per line in memory */
static uint32_t ytop;                   /* line in memory where text row 0 starts */
static int cur_row = -1, cur_col;       /* where the cursor is drawn (-1: nowhere) */
static int fresh;                       /* the screen is all black (just after a mode set) */

/* The 16 VGA text colors as 0x00RRGGBB (XRGB8888). */
static int swap_rb;                     /* GOP RGBX: R and B are swapped */

static const uint32_t palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* Plain (not volatile) pointers into the framebuffer: it is memory with no
   side effects, so the compiler may combine and order the stores freely —
   and with write-combining the CPU does the same. */
static uint32_t *pixel(uint32_t x, uint32_t y)
{
    return (uint32_t *)info->screen_base + y * pitch + x;
}

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t i = 0; i < h; i++) {
        uint32_t *p = pixel(x, y + i);
        for (uint32_t j = 0; j < w; j++)
            p[j] = color;
    }
}

/* One glyph at scale S (a constant in each copy below): every font pixel
   becomes an S x S square, stored straight from a register — no buffer, no
   copying, no loop counters once the compiler has unrolled the constant
   loops. That makes it about one instruction per screen pixel, which
   matters: a full 1920x1200 redraw is 2.3 million pixels. */
static inline __attribute__((always_inline))
void glyph(uint32_t *p, const uint8_t *g, uint32_t fg, uint32_t bg, int cursor, const int S)
{
    for (int y = 0; y < 16; y++, p += S * pitch) {
        unsigned bits = (cursor && y >= 14) ? 0xFF : g[y];
#pragma GCC unroll 8
        for (int x = 0; x < 8; x++) {
            uint32_t px = (bits & (0x80u >> x)) ? fg : bg;
#pragma GCC unroll 4
            for (int r = 0; r < S; r++)
#pragma GCC unroll 4
                for (int k = 0; k < S; k++)
                    p[r * pitch + x * S + k] = px;
        }
    }
}

/* Draw cell (r, c) from the grid, with the cursor if `cursor`. */
static void draw_cell(int r, int c, int cursor)
{
    uint16_t v = vt_grid()[r * VT_MAX_COLS + c];
    const uint8_t *g = font + (v & 0xFF) * 16;
    uint32_t fg = palette[(v >> 8) & 15], bg = palette[(v >> 12) & 15];
    if (swap_rb) {
        fg = (fg & 0xFF00FF00u) | ((fg & 0xFFu) << 16) | ((fg >> 16) & 0xFFu);
        bg = (bg & 0xFF00FF00u) | ((bg & 0xFFu) << 16) | ((bg >> 16) & 0xFFu);
    }
    uint32_t *p = pixel((uint32_t)(c * cw), ytop + (uint32_t)(r * ch));
    switch (scale) {
    case 1:  glyph(p, g, fg, bg, cursor, 1); break;
    case 2:  glyph(p, g, fg, bg, cursor, 2); break;
    case 3:  glyph(p, g, fg, bg, cursor, 3); break;
    default: glyph(p, g, fg, bg, cursor, 4); break;
    }
}

/* Black around the text: the strip right of the last column and below
   the last row (the screen is rarely an exact number of cells). */
static void clear_margins(void)
{
    uint32_t tw = (uint32_t)(cols * cw), th = (uint32_t)(rows * ch);
    fill(tw, ytop, info->var.xres - tw, th, 0);
    fill(0, ytop + th, info->var.xres, info->var.yres - th, 0);
}

static void pan_to(uint32_t y)
{
    struct fb_var_screeninfo var = info->var;
    var.xoffset = 0;
    var.yoffset = y;
    fb_pan_display(info, &var);
}

/* --- the vt.c backend ------------------------------------------------------ */

static void fbcon_draw(int row, int col, int n)
{
    const uint16_t *cells = vt_grid() + row * VT_MAX_COLS;
    for (int i = 0; i < n; i++) {
        int c = col + i, cursor = row == cur_row && c == cur_col;
        /* On a black screen an empty black cell is already drawn: at boot
           that's most of the screen, and skipping it makes switching to
           graphics several times cheaper. */
        if (fresh && !cursor && (cells[c] & 0xF0FF) == ' ')
            continue;
        draw_cell(row, c, cursor);
    }
}

static void fbcon_cursor(int row, int col)
{
    if (row == cur_row && col == cur_col)
        return;
    if (cur_row >= 0)
        draw_cell(cur_row, cur_col, 0);         /* take it away */
    cur_row = row;
    cur_col = col;
    draw_cell(row, col, 1);
}

static void fbcon_scroll(void)
{
    uint32_t yres = info->var.yres;
    if (ytop + (uint32_t)ch + yres > info->var.yres_virtual) {
        /* The end of video memory: back to line 0, draw it all once. */
        ytop = 0;
        pan_to(0);
        clear_margins();
        for (int r = 0; r < rows - 1; r++)
            for (int c = 0; c < cols; c++)
                draw_cell(r, c, 0);
        cur_row = -1;
        return;                                 /* vt.c draws the bottom row */
    }
    ytop += (uint32_t)ch;
    pan_to(ytop);
    /* The lines that just came into view held whatever was drawn there
       last time around: clear the new bottom row's right strip and the
       bottom margin (vt.c draws the row's cells). */
    uint32_t tw = (uint32_t)(cols * cw), th = (uint32_t)(rows * ch);
    fill(tw, ytop + th - (uint32_t)ch, info->var.xres - tw, (uint32_t)ch, 0);
    fill(0, ytop + th, info->var.xres, yres - th, 0);
    /* The cursor moved up with the picture: draw that cell plain. */
    if (cur_row > 0)
        draw_cell(cur_row - 1, cur_col, 0);
    cur_row = -1;
}

/* A program had the screen (KD_GRAPHICS): it may have panned or drawn
   anywhere. Start from line 0; vt.c draws every cell next. */
static void fbcon_resume(void)
{
    ytop = 0;
    pan_to(0);
    clear_margins();
    cur_row = -1;
}

static const struct vt_ops fbcon_ops = {
    .draw = fbcon_draw, .scroll = fbcon_scroll, .cursor = fbcon_cursor,
    .resume = fbcon_resume,
};

/* --- attaching ----------------------------------------------------------- */

/* Pick the font scale: about 120 columns on a 1920-wide screen (2x), the
   plain 8x16 font up to 1919 pixels — and never more cells than vt.c's
   grid holds. */
static void measure(void)
{
    uint32_t xres = info->var.xres, yres = info->var.yres;
    scale = (int)(xres / 960);
    if (scale < 1)
        scale = 1;
    if (scale > MAX_SCALE)
        scale = MAX_SCALE;
    while (scale < MAX_SCALE &&
           (xres / (8u * scale) > VT_MAX_COLS || yres / (16u * scale) > VT_MAX_ROWS))
        scale++;
    cw = 8 * scale;
    ch = 16 * scale;
    cols = (int)(xres / (uint32_t)cw);
    rows = (int)(yres / (uint32_t)ch);
    if (cols > VT_MAX_COLS)
        cols = VT_MAX_COLS;
    if (rows > VT_MAX_ROWS)
        rows = VT_MAX_ROWS;
    pitch = info->fix.line_length / 4;
}

/* Black first, then only the cells with something in them are drawn. A
   mode set leaves the screen black (see drm_simple_kms_helper.h), but the
   UEFI framebuffer (simplefb) has no mode set: it still holds the loader's
   text, which the blank cells would leave showing. One fill costs about
   one glyph's work per cell; drawing every blank cell would cost them all. */
static void take_over(void)
{
    measure();
    ytop = 0;
    cur_row = -1;
    pan_to(0);
    fill(0, 0, info->var.xres, info->var.yres, 0);
    fresh = 1;
    vt_set_backend(&fbcon_ops, cols, rows);     /* draws the grid */
    fresh = 0;
}

void fbcon_attach(struct fb_info *fb)
{
    const struct bootinfo *bi = P2V(BOOTINFO_PHYS);
    if (!bi->font_phys) {
        kprintf("fbcon: no font from the boot loader\n");
        return;
    }
    if (fb->var.bits_per_pixel != 32)
        return;
    font = P2V(bi->font_phys);
    swap_rb = (bi->fb_magic == BOOTINFO_FB_MAGIC && bi->fb_format == 1);
    info = fb;
    take_over();
    klog("fbcon: %dx%d\n", cols, rows);
}

void fbcon_mode_changed(struct fb_info *fb)
{
    if (fb == info && fb->var.bits_per_pixel == 32)
        take_over();
}
