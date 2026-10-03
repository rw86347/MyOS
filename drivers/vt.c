/*
 * vt.c — the console: a small VT100 terminal on any screen
 *
 * Programs don't just print text: shells redraw their input line with
 * "escape sequences" — ESC [ 3 D means "cursor 3 left", ESC [ K "erase to
 * the end of the line", ESC [ 1;32m "bright green". Real terminals (the
 * VT100, and the Linux console that programs expect with TERM=linux)
 * interpret them, so this console does too: the common ones below.
 *
 * The text lives in RAM, in a grid of 16-bit cells (character in the low
 * byte, color attribute in the high byte — the VGA text screen's format).
 * A backend shows the grid: vga.c copies cells to the text screen, fbcon.c
 * draws them as pixels. Keeping the grid here, not on the screen, means a
 * backend never reads video memory (very slow on real cards), the console
 * survives a switch from text to graphics mode, and a program that takes
 * over the screen (KD_GRAPHICS) gets the console back intact.
 *
 * The grid's row stride is always VT_MAX_COLS, whatever the screen size,
 * so a resize never has to re-pack the rows.
 */
#include <interrupts.h>
#include <string.h>
#include <tty.h>
#include <vt.h>

#define DEFAULT_COLOR (VT_LIGHT_GREY | (VT_BLACK << 4))

static uint16_t grid[VT_MAX_ROWS * VT_MAX_COLS];
static int cols, rows;
static const struct vt_ops *ops;
static int graphics;                /* KD_GRAPHICS: a program owns the screen */

static int row, col;
static int wrap_pending;            /* wrote the last column; wrap on the next char */
static uint8_t color = DEFAULT_COLOR;
static int bold, reverse;
static int saved_row, saved_col;

/* Escape-sequence parser state. */
static enum { NORMAL, ESC, CSI } state;
static int params[8], nparams, have_param;

static void (*reply)(const char *, int);    /* answers to "where is the cursor?" */

static uint16_t *at(int r, int c)
{
    return &grid[r * VT_MAX_COLS + c];
}

static uint16_t cell(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

static uint16_t blank(void)
{
    return cell(' ', color);
}

/* Tell the backend cells [c, c + n) of row r changed. */
static void changed(int r, int c, int n)
{
    if (ops && !graphics && n > 0)
        ops->draw(r, c, n);
}

static void changed_rows(int from, int to)  /* rows [from, to) */
{
    for (int r = from; r < to; r++)
        changed(r, 0, cols);
}

static void show_cursor(void)
{
    if (ops && !graphics)
        ops->cursor(row, col < cols ? col : cols - 1);
}

const uint16_t *vt_grid(void) { return grid; }
int vt_cols(void) { return cols; }
int vt_rows(void) { return rows; }

void vt_set_color(enum vt_color fg, enum vt_color bg)
{
    color = (uint8_t)(fg | (bg << 4));
}

void vt_set_reply(void (*fn)(const char *, int))
{
    reply = fn;
}

/* Blank cells [c0, c1) of row r. */
static void clear_cells(int r, int c0, int c1)
{
    uint16_t b = blank();
    uint16_t *p = at(r, 0);
    for (int c = c0; c < c1; c++)
        p[c] = b;
    changed(r, c0, c1 - c0);
}

static void clear_rows(int from, int to)
{
    for (int r = from; r < to; r++)
        clear_cells(r, 0, cols);
}

void vt_clear(void)
{
    uint32_t flags = irq_save();
    clear_rows(0, rows);
    row = col = 0;
    wrap_pending = 0;
    show_cursor();
    irq_restore(flags);
}

/* Move every line up by one and blank the bottom line. */
static void scroll(void)
{
    memmove(grid, at(1, 0), (size_t)(rows - 1) * VT_MAX_COLS * sizeof grid[0]);
    uint16_t b = blank();
    uint16_t *last = at(rows - 1, 0);
    for (int c = 0; c < cols; c++)
        last[c] = b;
    if (ops && !graphics)
        ops->scroll();
    changed(rows - 1, 0, cols);
    row = rows - 1;
}

static void move_to(int r, int c)
{
    row = r < 0 ? 0 : r >= rows ? rows - 1 : r;
    col = c < 0 ? 0 : c >= cols ? cols - 1 : c;
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
        fg = VT_LIGHT_GREY, bg = VT_BLACK, bold = 0, reverse = 0;
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
        fg = VT_LIGHT_GREY;
    } else if (p >= 40 && p <= 47) {
        bg = ansi_to_vga[p - 40];
    } else if (p == 49) {
        bg = VT_BLACK;
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
    uint16_t *line = at(row, 0);
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
        if (param(0, 0) == 0) {
            clear_cells(row, col, cols);
            clear_rows(row + 1, rows);
        } else if (param(0, 0) == 1) {
            clear_rows(0, row);
            clear_cells(row, 0, col + 1);
        } else {
            clear_rows(0, rows);
        }
        break;
    case 'K':                                               /* erase line */
        if (param(0, 0) == 0)
            clear_cells(row, col, cols);
        else if (param(0, 0) == 1)
            clear_cells(row, 0, col + 1);
        else
            clear_cells(row, 0, cols);
        break;
    case 'P':                                               /* delete chars */
        for (int x = col; x < cols; x++)
            line[x] = x + n < cols ? line[x + n] : blank();
        changed(row, col, cols - col);
        break;
    case '@':                                               /* insert blanks */
        for (int x = cols - 1; x >= col; x--)
            line[x] = x - n >= col ? line[x - n] : blank();
        changed(row, col, cols - col);
        break;
    case 'X':                                               /* erase chars */
        clear_cells(row, col, col + n < cols ? col + n : cols);
        break;
    case 'L':                                               /* insert lines */
    case 'M': {                                             /* delete lines */
        if (n > rows - row)
            n = rows - row;
        size_t stride = VT_MAX_COLS * sizeof grid[0];
        if (final == 'M') {
            memmove(at(row, 0), at(row + n, 0), (size_t)(rows - row - n) * stride);
            clear_rows(rows - n, rows);
            changed_rows(row, rows - n);
        } else {
            memmove(at(row + n, 0), at(row, 0), (size_t)(rows - row - n) * stride);
            clear_rows(row, row + n);
            changed_rows(row + n, rows);
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
            if (v >= 100) r[k++] = '0' + v / 100;
            if (v >= 10) r[k++] = '0' + v / 10 % 10;
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

/* Put one character (or escape-sequence byte), without moving the cursor. */
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
            clear_rows(0, rows);
            row = col = 0;
            wrap_pending = 0;
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
        if (col >= cols)
            col = cols - 1;
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
            if (row == rows)
                scroll();
        }
        *at(row, col) = cell(c, color);
        changed(row, col, 1);
        if (col == cols - 1)
            wrap_pending = 1;   /* like the Linux console: wrap on the next char */
        else
            col++;
        break;
    }
    if (row == rows)
        scroll();
}

/* Many characters, one cursor move: moving the cursor costs port writes
   (text screen) or redrawing two cells (graphics), so we do it once per
   piece of text, not per character. */
void vt_write(const char *s, size_t n)
{
    if (!rows)
        return;                 /* no screen yet */
    for (size_t i = 0; i < n; i++)
        put_char(s[i]);
    show_cursor();
}

void vt_write_at(int x, int y, const char *s, enum vt_color fg, enum vt_color bg)
{
    if (y >= rows || x < 0)
        return;
    uint8_t attr = (uint8_t)(fg | (bg << 4));
    int x0 = x;
    uint32_t flags = irq_save();
    for (; *s && x < cols; s++, x++)
        *at(y, x) = cell(*s, attr);
    changed(y, x0, x - x0);
    irq_restore(flags);
}

/* --- screens ------------------------------------------------------------------- */

void vt_set_backend(const struct vt_ops *new_ops, int nc, int nr)
{
    if (nc > VT_MAX_COLS)
        nc = VT_MAX_COLS;
    if (nr > VT_MAX_ROWS)
        nr = VT_MAX_ROWS;
    uint32_t flags = irq_save();

    /* A shorter screen keeps the lines up to the cursor: drop lines off
       the top until the cursor's line fits. */
    int shift = row >= nr ? row - nr + 1 : 0;
    int kept = rows - shift;            /* rows of old text still shown */
    if (shift)
        memmove(grid, at(shift, 0), (size_t)kept * VT_MAX_COLS * sizeof grid[0]);
    row -= shift;
    saved_row = saved_row >= shift ? saved_row - shift : 0;

    /* Blank what the old screen didn't have: new columns, new rows. */
    uint16_t b = blank();
    for (int r = 0; r < nr; r++)
        for (int c = r < kept ? cols : 0; c < nc; c++)
            *at(r, c) = b;

    cols = nc;
    rows = nr;
    if (col >= cols)
        col = cols - 1;
    if (saved_row >= rows)
        saved_row = rows - 1;
    if (saved_col >= cols)
        saved_col = cols - 1;
    wrap_pending = 0;
    ops = new_ops;
    changed_rows(0, rows);
    show_cursor();
    irq_restore(flags);

    tty_set_winsize(rows, cols);
}

void vt_set_graphics(int on)
{
    uint32_t flags = irq_save();
    int was = graphics;
    graphics = on;
    if (!on && was) {                   /* the console's turn again */
        if (ops && ops->resume)
            ops->resume();
        changed_rows(0, rows);
        show_cursor();
    }
    irq_restore(flags);
}

int vt_graphics(void)
{
    return graphics;
}
