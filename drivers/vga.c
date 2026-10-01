/*
 * vga.c — VGA text-mode screen output
 *
 * At boot the BIOS leaves the video card in 80x25 color text mode. The screen
 * is simply memory at physical address 0xB8000: 2000 cells of 2 bytes each,
 * row by row. Writing to that memory makes characters appear immediately.
 *
 *   byte 0: the ASCII character
 *   byte 1: the color attribute — low 4 bits foreground, high 4 bits background
 *
 * Without paging enabled, a pointer value is a physical address, so we can
 * just point at 0xB8000.
 */
#include <stddef.h>
#include <vga.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static volatile uint16_t *const vga_buffer = (uint16_t *)0xB8000;
static size_t  row, col;
static uint8_t color = VGA_LIGHT_GREY | (VGA_BLACK << 4);

static uint16_t cell(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    color = (uint8_t)(fg | (bg << 4));
}

void vga_clear(void)
{
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga_buffer[i] = cell(' ', color);
    row = col = 0;
}

/* Move every line up by one and blank the bottom line. */
static void scroll(void)
{
    for (size_t i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        vga_buffer[i] = vga_buffer[i + VGA_WIDTH];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = cell(' ', color);
    row = VGA_HEIGHT - 1;
}

void vga_putc(char c)
{
    if (c == '\n') {
        col = 0;
        row++;
    } else {
        vga_buffer[row * VGA_WIDTH + col] = cell(c, color);
        if (++col == VGA_WIDTH) {
            col = 0;
            row++;
        }
    }
    if (row == VGA_HEIGHT)
        scroll();
}

void vga_write(const char *s)
{
    while (*s)
        vga_putc(*s++);
}
