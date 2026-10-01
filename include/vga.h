#ifndef VGA_H
#define VGA_H

#include <stddef.h>
#include <stdint.h>

/* The 16 VGA text-mode colors. */
enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
    VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN,
    VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE,
};

void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putc(char c);
void vga_write(const char *s);

/* Write a string at a fixed screen position (column x, row y) without moving
   the cursor — for status displays like the uptime clock. */
void vga_write_at(size_t x, size_t y, const char *s, enum vga_color fg, enum vga_color bg);

#endif
