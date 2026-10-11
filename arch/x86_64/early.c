/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * early.c — first C in the 64-bit kernel
 *
 * Serial (COM1) is for QEMU. Real UEFI laptops have no COM1 you can
 * see, so we also paint the GOP the loader left in bootinfo. We are
 * still on the firmware identity map: a 64-bit write to 0x400000000
 * is just a pointer, not PSE-36.
 */
#include <bootinfo.h>
#include <io.h>
#include <stdint.h>

#define COM1     0x3F8
#define LSR_THRE 0x20

#define BG   0x00101828u
#define FG   0x00F0C040u
#define BAR  0x00C06020u

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void putc(char c)
{
    int spins = 100000;
    while ((inb(COM1 + 5) & LSR_THRE) == 0 && --spins)
        ;
    outb(COM1, (uint8_t)c);
}

static void puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            putc('\r');
        putc(*s);
    }
}

static void fill(const struct bootinfo *bi, uint32_t color)
{
    uint32_t y, x;
    uint8_t *row;

    if (!bi->fb_phys || !bi->fb_pitch || !bi->fb_width || !bi->fb_height)
        return;
    row = (uint8_t *)bi->fb_phys;
    for (y = 0; y < bi->fb_height; y++) {
        uint32_t *p = (uint32_t *)row;
        for (x = 0; x < bi->fb_width; x++)
            p[x] = color;
        row += bi->fb_pitch;
    }
}

static void bar(const struct bootinfo *bi, uint32_t y0, uint32_t rows, uint32_t color)
{
    uint32_t y, x;

    if (!bi->fb_phys)
        return;
    if (y0 >= bi->fb_height)
        return;
    if (y0 + rows > bi->fb_height)
        rows = bi->fb_height - y0;
    for (y = 0; y < rows; y++) {
        uint32_t *p = (uint32_t *)(bi->fb_phys + (uint64_t)(y0 + y) * bi->fb_pitch);
        for (x = 0; x < bi->fb_width; x++)
            p[x] = color;
    }
}

static void putpix(const struct bootinfo *bi, uint32_t x, uint32_t y, uint32_t color)
{
    uint32_t *p;

    if (x >= bi->fb_width || y >= bi->fb_height)
        return;
    p = (uint32_t *)(bi->fb_phys + (uint64_t)y * bi->fb_pitch + (uint64_t)x * 4);
    *p = color;
}

/* 2× scale so 8×16 glyphs read on a 1080p phone photo. */
static void glyph(const struct bootinfo *bi, uint32_t x, uint32_t y, unsigned char c,
                  uint32_t fg, uint32_t bg)
{
    const uint8_t *g;
    uint32_t row, col, dy, dx;

    if (!bi->font_phys)
        return;
    g = (const uint8_t *)bi->font_phys + (uint32_t)c * 16;
    for (row = 0; row < 16; row++) {
        uint8_t bits = g[row];
        for (col = 0; col < 8; col++) {
            uint32_t color = (bits & (uint8_t)(0x80u >> col)) ? fg : bg;
            for (dy = 0; dy < 2; dy++)
                for (dx = 0; dx < 2; dx++)
                    putpix(bi, x + col * 2 + dx, y + row * 2 + dy, color);
        }
    }
}

static void text(const struct bootinfo *bi, uint32_t x0, uint32_t y, const char *s,
                 uint32_t fg, uint32_t bg)
{
    uint32_t x = x0;

    for (; *s; s++) {
        if (*s == '\n') {
            y += 36;
            x = x0;
            continue;
        }
        glyph(bi, x, y, (unsigned char)*s, fg, bg);
        x += 16;
    }
}

void kernel_main(void)
{
    const struct bootinfo *bi = (const struct bootinfo *)BOOTINFO_PHYS;

    serial_init();
    puts("Hello world from MyOS!  (long mode, UEFI)\n");

    if (bi->loader_magic == BOOTINFO_MAGIC && bi->fb_magic == BOOTINFO_FB_MAGIC) {
        fill(bi, BG);
        bar(bi, 0, 48, BAR);
        text(bi, 48, 80,
             "Hello world from MyOS!\n"
             "long mode  x86_64",
             FG, BG);
    }

    for (;;)
        __asm__ volatile ("hlt");
}
