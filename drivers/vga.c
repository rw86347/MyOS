/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * vga.c — the console on the VGA text screen (the vt.c backend for 80x25)
 *
 * At boot the BIOS leaves the video card in 80x25 color text mode. The screen
 * is simply memory at physical address 0xB8000: 2000 cells of 2 bytes each,
 * row by row. Writing to that memory makes characters appear immediately.
 *
 *   byte 0: the ASCII character
 *   byte 1: the color attribute — low 4 bits foreground, high 4 bits background
 *
 * That is exactly the format of vt.c's grid, so drawing is a copy.
 * 0xB8000 is a PHYSICAL address. With paging on, the kernel reaches physical
 * memory through the direct map at +0xC0000000 (see memlayout.h).
 *
 * This is the console on every machine until a graphics driver takes over
 * (drivers/gpu/drm), and the only one on machines without a driver.
 */
#include <io.h>
#include <memlayout.h>
#include <vga.h>
#include <vt.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static volatile uint16_t *const vga_buffer = P2V(0xB8000);

static void text_draw(int row, int col, int n)
{
    const uint16_t *src = vt_grid() + row * VT_MAX_COLS + col;
    volatile uint16_t *dst = vga_buffer + row * VGA_WIDTH + col;
    for (int i = 0; i < n; i++)
        dst[i] = src[i];
}

/* The grid already moved: copy it all (4000 bytes). Reading the old screen
   back to move it would be slower — reads from video memory are. */
static void text_scroll(void)
{
    for (int r = 0; r < VGA_HEIGHT - 1; r++)
        text_draw(r, 0, VGA_WIDTH);
}

/* Move the blinking hardware cursor to (row, col). The VGA card's CRT
   controller is programmed through an index port (0x3D4) and a data port
   (0x3D5): registers 0x0E/0x0F hold the cursor position, high/low byte. */
static void text_cursor(int row, int col)
{
    uint16_t pos = row * VGA_WIDTH + col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

static const struct vt_ops text_ops = {
    .draw = text_draw, .scroll = text_scroll, .cursor = text_cursor,
};

void vga_text_init(void)
{
    vt_set_backend(&text_ops, VGA_WIDTH, VGA_HEIGHT);
}
