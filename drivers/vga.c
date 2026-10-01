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
 * 0xB8000 is a PHYSICAL address. With paging on, the kernel reaches physical
 * memory through the direct map at +0xC0000000 (see memlayout.h).
 */
#include <io.h>
#include <memlayout.h>
#include <stddef.h>
#include <vga.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static volatile uint16_t *const vga_buffer = P2V(0xB8000);
static size_t  row, col;
static uint8_t color = VGA_LIGHT_GREY | (VGA_BLACK << 4);

static void update_cursor(void);

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
    update_cursor();
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

/* Move the blinking hardware cursor to (row, col). The VGA card's CRT
   controller is programmed through an index port (0x3D4) and a data port
   (0x3D5): registers 0x0E/0x0F hold the cursor position, high/low byte. */
static void update_cursor(void)
{
    uint16_t pos = row * VGA_WIDTH + col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

/* Put one character, without moving the hardware cursor. */
static void put_char(char c)
{
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\b') {     /* backspace: step back and erase */
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = VGA_WIDTH - 1;
        }
        vga_buffer[row * VGA_WIDTH + col] = cell(' ', color);
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

void vga_putc(char c)
{
    put_char(c);
    update_cursor();
}

/* Many characters, one cursor move: the cursor is set through 4 slow port
   writes, so we do that once per piece of text, not per character. */
void vga_write_n(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        put_char(s[i]);
    update_cursor();
}

void vga_write_at(size_t x, size_t y, const char *s, enum vga_color fg, enum vga_color bg)
{
    uint8_t attr = (uint8_t)(fg | (bg << 4));
    for (; *s && x < VGA_WIDTH; s++, x++)
        vga_buffer[y * VGA_WIDTH + x] = cell(*s, attr);
}

void vga_write(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    vga_write_n(s, n);
}
