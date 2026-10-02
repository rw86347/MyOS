/*
 * vga.c — VGA text-mode screen output, with a small VT100 terminal
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
 *
 * Programs don't just print text: shells redraw their input line with
 * "escape sequences" — ESC [ 3 D means "cursor 3 left", ESC [ K "erase to
 * the end of the line", ESC [ 1;32m "bright green". Real terminals (the
 * VT100, and the Linux console that programs expect with TERM=linux)
 * interpret them, so this screen does too: the common ones below.
 */
#include <io.h>
#include <memlayout.h>
#include <stddef.h>
#include <vga.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define DEFAULT_COLOR (VGA_LIGHT_GREY | (VGA_BLACK << 4))

static volatile uint16_t *const vga_buffer = P2V(0xB8000);
static int row, col;
static int wrap_pending;            /* wrote the last column; wrap on the next char */
static uint8_t color = DEFAULT_COLOR;
static int bold, reverse;
static int saved_row, saved_col;

/* Escape-sequence parser state. */
static enum { NORMAL, ESC, CSI } state;
static int params[8], nparams, have_param;

static void (*reply)(const char *, int);    /* answers to "where is the cursor?" */

static void update_cursor(void);

static uint16_t cell(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

static uint16_t blank(void)
{
    return cell(' ', color);
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    color = (uint8_t)(fg | (bg << 4));
}

void vga_set_reply(void (*fn)(const char *, int))
{
    reply = fn;
}

static void clear_range(int from, int to)  /* cells [from, to) */
{
    for (int i = from; i < to; i++)
        vga_buffer[i] = blank();
}

void vga_clear(void)
{
    clear_range(0, VGA_WIDTH * VGA_HEIGHT);
    row = col = 0;
    wrap_pending = 0;
    update_cursor();
}

/* Move every line up by one and blank the bottom line. */
static void scroll(void)
{
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        vga_buffer[i] = vga_buffer[i + VGA_WIDTH];
    clear_range((VGA_HEIGHT - 1) * VGA_WIDTH, VGA_HEIGHT * VGA_WIDTH);
    row = VGA_HEIGHT - 1;
}

/* Move the blinking hardware cursor to (row, col). The VGA card's CRT
   controller is programmed through an index port (0x3D4) and a data port
   (0x3D5): registers 0x0E/0x0F hold the cursor position, high/low byte. */
static void update_cursor(void)
{
    uint16_t pos = row * VGA_WIDTH + (col < VGA_WIDTH ? col : VGA_WIDTH - 1);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

static void move_to(int r, int c)
{
    row = r < 0 ? 0 : r >= VGA_HEIGHT ? VGA_HEIGHT - 1 : r;
    col = c < 0 ? 0 : c >= VGA_WIDTH ? VGA_WIDTH - 1 : c;
    wrap_pending = 0;
}

/* --- SGR: colors ("ESC [ ... m") ------------------------------------------- */

/* ANSI color order (black red green yellow blue magenta cyan white)
   -> VGA palette numbers. */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void set_attr(int p)
{
    uint8_t fg = color & 0x0F, bg = color >> 4;
    if (reverse) {                      /* work in un-reversed colors */
        uint8_t t = fg; fg = bg; bg = t;
    }
    if (p == 0) {
        fg = VGA_LIGHT_GREY, bg = VGA_BLACK, bold = 0, reverse = 0;
    } else if (p == 1) {
        bold = 1;
    } else if (p == 22) {
        bold = 0;
        fg &= 7;
    } else if (p == 7) {
        reverse = 1;
    } else if (p == 27) {
        reverse = 0;
    } else if (p >= 30 && p <= 37) {
        fg = ansi_to_vga[p - 30];
    } else if (p == 39) {
        fg = VGA_LIGHT_GREY;
    } else if (p >= 40 && p <= 47) {
        bg = ansi_to_vga[p - 40];
    } else if (p == 49) {
        bg = VGA_BLACK;
    } else if (p >= 90 && p <= 97) {
        fg = ansi_to_vga[p - 90] | 8;
    } else if (p >= 100 && p <= 107) {
        bg = ansi_to_vga[p - 100] | 8;
    }
    if (bold)
        fg |= 8;                        /* bold = the bright version */
    if (reverse) {
        uint8_t t = fg; fg = bg; bg = t;
    }
    color = (uint8_t)(fg | (bg << 4));
}

/* --- CSI sequences ("ESC [ params final") ------------------------------------- */

static int param(int i, int dflt)
{
    return (i < nparams && params[i]) ? params[i] : dflt;
}

static void csi(char final)
{
    int line = row * VGA_WIDTH;
    int n = param(0, 1);
    switch (final) {
    case 'A': move_to(row - n, col); break;                 /* cursor up */
    case 'B': move_to(row + n, col); break;                 /* down */
    case 'C': move_to(row, col + n); break;                 /* right */
    case 'D': move_to(row, col - n); break;                 /* left */
    case 'G': move_to(row, n - 1); break;                   /* to column */
    case 'd': move_to(n - 1, col); break;                   /* to row */
    case 'H': case 'f':                                     /* to row;col */
        move_to(param(0, 1) - 1, param(1, 1) - 1);
        break;
    case 'J':                                               /* erase screen */
        if (param(0, 0) == 0)
            clear_range(line + col, VGA_WIDTH * VGA_HEIGHT);
        else if (param(0, 0) == 1)
            clear_range(0, line + col + 1);
        else
            clear_range(0, VGA_WIDTH * VGA_HEIGHT);
        break;
    case 'K':                                               /* erase line */
        if (param(0, 0) == 0)
            clear_range(line + col, line + VGA_WIDTH);
        else if (param(0, 0) == 1)
            clear_range(line, line + col + 1);
        else
            clear_range(line, line + VGA_WIDTH);
        break;
    case 'P':                                               /* delete chars */
        for (int x = col; x < VGA_WIDTH; x++)
            vga_buffer[line + x] = x + n < VGA_WIDTH ? vga_buffer[line + x + n] : blank();
        break;
    case '@':                                               /* insert blanks */
        for (int x = VGA_WIDTH - 1; x >= col; x--)
            vga_buffer[line + x] = x - n >= col ? vga_buffer[line + x - n] : blank();
        break;
    case 'X':                                               /* erase chars */
        clear_range(line + col, line + (col + n < VGA_WIDTH ? col + n : VGA_WIDTH));
        break;
    case 'L':                                               /* insert lines */
    case 'M': {                                             /* delete lines */
        for (int k = 0; k < n; k++) {
            if (final == 'M') {
                for (int r = row; r < VGA_HEIGHT - 1; r++)
                    for (int x = 0; x < VGA_WIDTH; x++)
                        vga_buffer[r * VGA_WIDTH + x] = vga_buffer[(r + 1) * VGA_WIDTH + x];
                clear_range((VGA_HEIGHT - 1) * VGA_WIDTH, VGA_HEIGHT * VGA_WIDTH);
            } else {
                for (int r = VGA_HEIGHT - 1; r > row; r--)
                    for (int x = 0; x < VGA_WIDTH; x++)
                        vga_buffer[r * VGA_WIDTH + x] = vga_buffer[(r - 1) * VGA_WIDTH + x];
                clear_range(line, line + VGA_WIDTH);
            }
        }
        break;
    }
    case 'm':                                               /* colors */
        if (nparams == 0)
            set_attr(0);
        for (int i = 0; i < nparams; i++)
            set_attr(params[i]);
        break;
    case 's': saved_row = row; saved_col = col; break;
    case 'u': move_to(saved_row, saved_col); break;
    case 'n':                                               /* status report */
        if (param(0, 0) == 6 && reply) {    /* "where's the cursor?" */
            char r[16];
            int k = 0, v;
            r[k++] = 27; r[k++] = '[';
            v = row + 1;
            if (v >= 10) r[k++] = '0' + v / 10;
            r[k++] = '0' + v % 10;
            r[k++] = ';';
            v = col + 1;
            if (v >= 10) r[k++] = '0' + v / 10;
            r[k++] = '0' + v % 10;
            r[k++] = 'R';
            reply(r, k);
        }
        break;
    default:
        break;                  /* modes (h/l), scroll regions...: ignored */
    }
}

/* --- characters ------------------------------------------------------------------ */

/* Put one character (or escape-sequence byte), without moving the hardware
   cursor. */
static void put_char(char c)
{
    if (state == ESC) {
        state = NORMAL;
        if (c == '[') {
            state = CSI;
            nparams = 0;
            have_param = 0;
            params[0] = 0;
        } else if (c == '7') {
            saved_row = row, saved_col = col;
        } else if (c == '8') {
            move_to(saved_row, saved_col);
        } else if (c == 'c') {
            set_attr(0);
            vga_clear();
        }
        return;
    }
    if (state == CSI) {
        if (c >= '0' && c <= '9') {
            if (nparams == 0)
                nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (nparams == 0)
                nparams = 1;
            if (nparams < 8)
                params[nparams++] = 0;
        } else if (c == '?' || c == '>' || c == '=') {
            /* private sequences: same shape, mostly ignored */
        } else {
            state = NORMAL;
            csi(c);
        }
        return;
    }

    switch (c) {
    case 27:
        state = ESC;
        return;
    case '\n':
        col = 0;
        row++;
        wrap_pending = 0;
        break;
    case '\r':
        col = 0;
        wrap_pending = 0;
        return;
    case '\b':                  /* backspace moves left; it doesn't erase */
        if (wrap_pending)
            wrap_pending = 0;
        else if (col > 0)
            col--;
        return;
    case '\t':
        col = (col + 8) & ~7;
        if (col >= VGA_WIDTH)
            col = VGA_WIDTH - 1;
        return;
    case '\a':
        return;                 /* bell: nothing to ring */
    default:
        if ((uint8_t)c < 32)
            return;             /* other control characters: invisible */
        if (wrap_pending) {     /* the previous char filled the line */
            col = 0;
            row++;
            wrap_pending = 0;
            if (row == VGA_HEIGHT)
                scroll();
        }
        vga_buffer[row * VGA_WIDTH + col] = cell(c, color);
        if (col == VGA_WIDTH - 1)
            wrap_pending = 1;   /* like the Linux console: wrap on the next char */
        else
            col++;
        break;
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
