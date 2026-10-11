/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef VT_H
#define VT_H

#include <stddef.h>
#include <stdint.h>

/*
 * The console (vt.c): a VT100-style terminal on whatever screen there is.
 *
 * vt.c keeps the screen's text in RAM — a grid of cells, each one byte of
 * character and one byte of color, the same layout as the VGA text screen —
 * and interprets the escape sequences programs send. A backend puts the grid
 * on the screen: vga.c for the 80x25 text screen, fbcon.c for a graphics
 * framebuffer. The backend never has to read the screen back: everything it
 * draws comes from the grid.
 */

/* The 16 console colors (the VGA palette order). An attribute byte is
   fg | bg << 4. */
enum vt_color {
    VT_BLACK = 0, VT_BLUE, VT_GREEN, VT_CYAN, VT_RED, VT_MAGENTA,
    VT_BROWN, VT_LIGHT_GREY, VT_DARK_GREY, VT_LIGHT_BLUE, VT_LIGHT_GREEN,
    VT_LIGHT_CYAN, VT_LIGHT_RED, VT_LIGHT_MAGENTA, VT_YELLOW, VT_WHITE,
};

/* The grid's fixed row stride: row r starts at cell r * VT_MAX_COLS, at any
   size. 240x75 is 1920x1200 with an 8x16 font; fbcon picks a font scale
   that fits. */
#define VT_MAX_COLS 240
#define VT_MAX_ROWS 75

struct vt_ops {
    /* Cells [col, col + n) of row changed: draw them from the grid. */
    void (*draw)(int row, int col, int n);
    /* The whole grid moved up one line. The backend moves what is on the
       screen (or redraws it); the new bottom line arrives through draw. */
    void (*scroll)(void);
    /* The cursor is now at (row, col). */
    void (*cursor)(int row, int col);
    /* Optional: the console gets the screen back from a program (KD_TEXT),
       which may have changed it in any way; everything is drawn right after. */
    void (*resume)(void);
};

/* Put the console on a screen of cols x rows (at most VT_MAX_*). The text
   already there is kept (the bottom part, if the screen got shorter) and
   everything is drawn again. Programs on the console get SIGWINCH. */
void vt_set_backend(const struct vt_ops *ops, int cols, int rows);

/* The grid: cell (row, col) is vt_grid()[row * VT_MAX_COLS + col]. */
const uint16_t *vt_grid(void);
int vt_cols(void);
int vt_rows(void);

/* Text and escape sequences, as a program writes them. */
void vt_write(const char *s, size_t n);
void vt_clear(void);
void vt_set_color(enum vt_color fg, enum vt_color bg);

/* Write at a fixed position without moving the cursor (the uptime clock). */
void vt_write_at(int x, int y, const char *s, enum vt_color fg, enum vt_color bg);

/* Where the console's answers go (ESC [ 6 n: "where is the cursor?"): the
   terminal's input, like a real terminal. */
void vt_set_reply(void (*fn)(const char *, int));

/* KD_GRAPHICS: a program draws on the screen itself, so the console stops
   drawing (it still keeps its grid up to date). KD_TEXT: draw it all again. */
void vt_set_graphics(int on);
int  vt_graphics(void);

#endif
