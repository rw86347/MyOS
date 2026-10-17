/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * hid.c — HID report descriptors and reports: keyboards and mice
 *
 * A HID device ("Human Interface Device": keyboards, mice, touchpads, game
 * pads) sends REPORTS: a few bytes saying what changed. The layout differs
 * between devices, so each one describes its own in a REPORT DESCRIPTOR,
 * a little program of "items" of 1 to 5 bytes. A plain mouse's:
 *
 *   05 01        Usage Page (Generic Desktop)     what the usages below mean
 *   09 02        Usage (Mouse)
 *   a1 01        Collection (Application)         a mouse begins
 *   05 09          Usage Page (Button)
 *   19 01 29 03    Usage Minimum (1), Maximum (3)   buttons 1 to 3...
 *   75 01 95 03    Report Size (1), Count (3)
 *   81 02          Input (Data, Variable)         ...are three 1-bit fields
 *   75 05 95 01    Report Size (5), Count (1)
 *   81 01          Input (Constant)               5 bits of padding
 *   05 01 09 30 09 31 09 38                       X, Y, Wheel
 *   15 81 25 7f    Logical Minimum (-127), Maximum (127)
 *   75 08 95 03    Report Size (8), Count (3)
 *   81 06          Input (Data, Variable, Relative)
 *   c0           End Collection
 *
 * So that mouse sends 4 bytes: buttons, X, Y, wheel. Items are "global"
 * (Usage Page, Logical Min/Max, Report Size/Count, Report ID: they last
 * until changed), "local" (Usage, Usage Min/Max: for the next main item
 * only) or "main" (Input, Output, Feature, Collection). Each Input item
 * lays out count values of size bits, one after another. A device with
 * several kinds of report starts each with a Report ID byte.
 *
 * hid_parse runs the program once, when the device is found, and keeps
 * only the fields MyOS uses, with their bit positions: keys, buttons, X,
 * Y, the wheel. hid_report then picks those bits out of each report, in
 * the interrupt: a few shifts per field, no searching, no allocation.
 *
 * A keyboard reports the keys held NOW, not presses: an array of up to
 * six key "usages" (0x04 is A) and a bitmap of the eight modifiers. A
 * press or a release is a difference from the last report. Usages become
 * Linux key codes by Linux's own table (hid-input.c) and go to
 * keyboard_key, as PS/2 keys do. A mouse reports its movement since the
 * last report, which goes to mouse_report.
 *
 * A touch screen or a tablet (and QEMU's usb-tablet, which is how UTM
 * hands over the Mac's pointer) reports WHERE it points: absolute X and
 * Y, each in its own range, which is scaled to EVDEV_ABS_MAX, and a "tip
 * switch" (Digitizer page) that is on while a finger or pen touches. That
 * goes to /dev/input/event2 (evdev_touch). A multi-touch screen sends
 * several fingers per report; MyOS follows the first, so one finger
 * points and taps. Gestures need every finger (docs/TOUCHPAD-PLAN.md).
 *
 * Not yet: the Consumer page (volume keys), the keyboard's lights, and
 * touchpads' finger reports (docs/TOUCHPAD-PLAN.md; they send mouse
 * reports until they are switched over, which is what MyOS uses now).
 */
#include <evdev.h>
#include <hid.h>
#include <keyboard.h>
#include <mouse.h>
#include <string.h>

/* The fields kept. */
enum {
    HF_KEYS = 1,        /* keyboard usages, an array: each value is a key held */
    HF_KEYBITS,         /* keyboard usages, one bit per key (the modifiers) */
    HF_BUTTONS,         /* mouse buttons, one bit each */
    HF_X, HF_Y, HF_WHEEL,   /* movement (relative) */
    HF_ABSX, HF_ABSY,   /* where it points (absolute): the first X and Y */
    HF_TIP,             /* a finger or pen touches: the first tip switch */
};

#define PAGE_DESKTOP  0x01
#define PAGE_KEYBOARD 0x07
#define PAGE_BUTTON   0x09
#define PAGE_DIGITIZER 0x0D
#define DIGITIZER_TIP 0x42
#define DESKTOP_X     0x30
#define DESKTOP_Y     0x31
#define DESKTOP_WHEEL 0x38

#define USAGES      16                  /* local usages per main item */
#define REPORT_IDS  8                   /* different reports per device */

/* Keyboard usage (0x00-0x67) -> Linux key code: Linux's hid_keyboard[]
   (drivers/hid/hid-input.c). 0: none. */
static const uint8_t usage_key[0x68] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,    /* a-l */
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,    /* m-z, 1 2 */
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,    /* 3-0, Enter... */
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,    /* ] \ ; ' ` , . / Caps F1-F6 */
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,    /* F7-F12, Print... Right */
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,    /* Left Down Up, keypad */
     72, 73, 82, 83, 86,127,116,117,                                    /* keypad, 102nd, Menu */
};
/* The modifiers, usages 0xE0-0xE7: Ctrl, Shift, Alt, GUI; left, then right. */
static const uint8_t modifier_key[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

/* ---- the descriptor (once, at probe) ---- */

struct globals {
    uint16_t page, count;
    uint8_t  id, size;
    int32_t  lmin, lmax_s;
    uint32_t lmax_u;                    /* Logical Maximum, read both ways */
};

static void add(struct hid *h, int kind, const struct globals *g, uint32_t bit,
                uint32_t count, uint32_t usage)
{
    if (h->nf == HID_FIELDS || bit + count * g->size > 0xFFFF)
        return;
    if (kind >= HF_ABSX)                /* only the first finger's: one per report */
        for (int i = 0; i < h->nf; i++)
            if (h->f[i].kind == kind && h->f[i].id == g->id)
                return;
    struct hid_field *f = &h->f[h->nf++];
    f->id = g->id;
    f->kind = (uint8_t)kind;
    f->size = g->size;
    f->count = (uint16_t)count;
    f->bit = (uint16_t)bit;
    f->usage = (uint16_t)usage;
    f->lmin = g->lmin;
    /* The maximum is signed only if the minimum is negative (HID 6.2.2.7;
       "25 ff" with a minimum of 0 means 255). */
    f->lmax = g->lmin < 0 ? g->lmax_s : (int32_t)g->lmax_u;
    if (kind <= HF_KEYBITS)
        h->keyboard = 1;
    else if (kind == HF_X || kind == HF_Y)  /* (a wheel alone isn't a mouse: tablets have one) */
        h->mouse = 1;
    else if (kind >= HF_ABSX)
        h->touch = 1;
}

/* An Input item of values, starting at `bit`: keep the ones MyOS uses.
   Usages are page << 16 | id. */
static void input(struct hid *h, const struct globals *g, uint32_t flags, uint32_t bit,
                  const uint32_t *usage, int nu, uint32_t umin, uint32_t umax, int range)
{
    if (flags & 1)
        return;                         /* Constant: padding */
    if (!(flags & 2)) {                 /* an array: each value names a usage */
        uint32_t base = (range & 1) ? umin : nu ? usage[0] : 0;
        if (base >> 16 == PAGE_KEYBOARD)
            add(h, HF_KEYS, g, bit, g->count, base & 0xFFFF);
        return;
    }
    /* Variables, value i of usage min + i: keys and buttons are one field. */
    if (range == 3 && !nu && umax >= umin) {
        uint32_t n = umax - umin + 1;
        if (n > g->count)
            n = g->count;
        if (umin >> 16 == PAGE_KEYBOARD) {
            add(h, HF_KEYBITS, g, bit, n, umin & 0xFFFF);
            return;
        }
        if (umin >> 16 == PAGE_BUTTON) {
            add(h, HF_BUTTONS, g, bit, n, umin & 0xFFFF);
            return;
        }
    }
    /* Or one usage per value, from the list (the last one repeats). */
    for (uint32_t i = 0; i < g->count && i < 64; i++) {
        uint32_t u = nu ? usage[(int)i < nu ? (int)i : nu - 1] : (range & 1) ? umin + i : 0;
        int kind = 0, rel = flags & 4;
        if (u >> 16 == PAGE_KEYBOARD)
            kind = HF_KEYBITS;
        else if (u >> 16 == PAGE_BUTTON)
            kind = HF_BUTTONS;
        else if (u == (PAGE_DESKTOP << 16 | DESKTOP_X))
            kind = rel ? HF_X : HF_ABSX;
        else if (u == (PAGE_DESKTOP << 16 | DESKTOP_Y))
            kind = rel ? HF_Y : HF_ABSY;
        else if (u == (PAGE_DESKTOP << 16 | DESKTOP_WHEEL) && rel)
            kind = HF_WHEEL;
        else if (u == (PAGE_DIGITIZER << 16 | DIGITIZER_TIP))
            kind = HF_TIP;
        if (kind)
            add(h, kind, g, bit + i * g->size, 1, u & 0xFFFF);
    }
}

int hid_parse(struct hid *h, const uint8_t *d, int len)
{
    memset(h, 0, sizeof *h);
    struct globals g = { 0 }, stack[4];
    int depth = 0;
    uint32_t usage[USAGES], umin = 0, umax = 0;
    int nu = 0, range = 0;              /* range: bit 0 Usage Minimum, bit 1 Maximum seen */
    /* The bits laid out so far in each report. */
    uint8_t id[REPORT_IDS] = { 0 };
    uint32_t off[REPORT_IDS] = { 0 };
    int nid = 1;

    for (int o = 0; o < len; ) {
        uint8_t p = d[o++];
        if (p == 0xFE) {                /* a long item (none are defined): skip it */
            if (o >= len)
                break;
            o += 2 + d[o];
            continue;
        }
        int sz = (p & 3) == 3 ? 4 : p & 3;
        if (o + sz > len)
            break;
        uint32_t u = 0;
        for (int i = 0; i < sz; i++)
            u |= (uint32_t)d[o + i] << (8 * i);
        int32_t sv = sz == 1 ? (int8_t)u : sz == 2 ? (int16_t)u : (int32_t)u;
        o += sz;
        /* A usage of 1 or 2 bytes is on the current page; 4 bytes carry their own. */
        uint32_t full = sz == 4 ? u : (uint32_t)g.page << 16 | (u & 0xFFFF);

        switch (p & 0xFC) {
        case 0x80: {                    /* Input */
            int k = 0;
            while (k < nid && id[k] != g.id)
                k++;
            if (k == nid && nid < REPORT_IDS)
                id[nid++] = g.id;
            if (k < nid) {
                if (g.size && g.size <= 32)
                    input(h, &g, u, off[k], usage, nu, umin, umax, range);
                off[k] += (uint32_t)g.size * g.count;
            }
        }   /* fall through */
        case 0x90: case 0xA0: case 0xB0: case 0xC0:     /* Output, Collection, Feature, End: */
            nu = range = 0;                             /* a main item ends the local items */
            break;
        case 0x04: g.page = (uint16_t)u; break;         /* Usage Page */
        case 0x14: g.lmin = sv; break;                  /* Logical Minimum */
        case 0x24: g.lmax_s = sv, g.lmax_u = u; break;  /* Logical Maximum */
        case 0x74: g.size = (uint8_t)(u > 255 ? 255 : u); break;     /* Report Size */
        case 0x84: g.id = (uint8_t)u, h->ids = 1; break;            /* Report ID */
        case 0x94: g.count = (uint16_t)(u > 0xFFFF ? 0xFFFF : u); break;   /* Report Count */
        case 0xA4: if (depth < 4) stack[depth++] = g; break;        /* Push */
        case 0xB4: if (depth) g = stack[--depth]; break;            /* Pop */
        case 0x08: if (nu < USAGES) usage[nu++] = full; break;      /* Usage */
        case 0x18: umin = full, range |= 1; break;                  /* Usage Minimum */
        case 0x28: umax = full, range |= 2; break;                  /* Usage Maximum */
        }
    }
    for (int k = 0; k < nid; k++)
        if ((off[k] + 7) / 8 + h->ids > h->bytes)
            h->bytes = (uint16_t)((off[k] + 7) / 8 + h->ids);
    return h->keyboard || h->mouse || h->touch ? 0 : -1;
}

/* ---- the reports (in the interrupt) ---- */

/* `size` bits (1-32) from bit `bit` on, least significant first. */
static uint32_t bits(const uint8_t *r, uint32_t bit, uint32_t size)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; ) {
        uint32_t shift = (bit + i) % 8, take = 8 - shift;
        if (take > size - i)
            take = size - i;
        v |= (uint32_t)((r[(bit + i) / 8] >> shift) & ((1u << take) - 1)) << i;
        i += take;
    }
    return v;
}

/* Value k of field f, sign-extended if the field can be negative. */
static int32_t value(const uint8_t *r, const struct hid_field *f, uint32_t k)
{
    uint32_t v = bits(r, f->bit + k * f->size, f->size);
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1)) & 1)
        v |= ~0u << f->size;
    return (int32_t)v;
}

/* The keys held now against those held before: a release or a press for
   each difference. The modifiers (byte 28: usages 0xE0-0xE7) go first,
   so that Shift and A pressed together are a capital A. */
static void keys_changed(struct hid *h, const uint8_t *now)
{
    for (int i = 0; i < 32; i++) {
        int b = i == 0 ? 28 : i <= 28 ? i - 1 : i;
        uint8_t diff = now[b] ^ h->keys[b];
        h->keys[b] = now[b];
        while (diff) {
            int bit = __builtin_ctz(diff);
            diff &= diff - 1;
            uint32_t u = (uint32_t)b * 8 + bit;
            uint16_t code = u < sizeof usage_key ? usage_key[u] :
                            u >= 0xE0 && u <= 0xE7 ? modifier_key[u - 0xE0] : 0;
            if (code)
                keyboard_key(code, (now[b] >> bit) & 1);
        }
    }
}

static int16_t clamp16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* An absolute value in its field's range, as 0..EVDEV_ABS_MAX. */
static int32_t scale(const struct hid_field *f, int32_t v)
{
    if (f->lmax <= f->lmin)
        return 0;
    if (v < f->lmin)
        v = f->lmin;
    if (v > f->lmax)
        v = f->lmax;
    return (int32_t)((uint64_t)(uint32_t)(v - f->lmin) * EVDEV_ABS_MAX /
                     (uint32_t)(f->lmax - f->lmin));
}

void hid_report(struct hid *h, const uint8_t *r, int n)
{
    uint8_t id = 0;
    if (h->ids) {
        if (n < 1)
            return;
        id = *r++;
        n--;
    }
    uint8_t keys[32];
    int kb = 0, phantom = 0, ms = 0, has_buttons = 0, abs = 0, tip = -1;
    uint8_t buttons = 0;
    int32_t dx = 0, dy = 0, wheel = 0, ax = -1, ay = -1;
    for (int i = 0; i < h->nf; i++) {
        const struct hid_field *f = &h->f[i];
        if (f->id != id || f->bit + (uint32_t)f->size * f->count > (uint32_t)n * 8)
            continue;                   /* another report's, or past what came */
        switch (f->kind) {
        case HF_KEYS:
        case HF_KEYBITS:
            if (!kb++)
                memset(keys, 0, sizeof keys);
            for (uint32_t k = 0; k < f->count; k++) {
                uint32_t u;
                if (f->kind == HF_KEYBITS) {
                    if (!bits(r, f->bit + k * f->size, f->size))
                        continue;
                    u = f->usage + k;
                } else {
                    int32_t v = value(r, f, k);
                    if (v < f->lmin || v > f->lmax)
                        continue;       /* out of range: no key */
                    u = f->usage + (uint32_t)(v - f->lmin);
                }
                if (u == 1)             /* ErrorRollOver: too many keys at once, */
                    phantom = 1;        /* and the report means nothing */
                else if (u > 3 && u < 256)
                    keys[u / 8] |= 1 << (u % 8);
            }
            break;
        case HF_BUTTONS:
            ms = has_buttons = 1;
            for (uint32_t k = 0; k < f->count; k++)
                if (bits(r, f->bit + k * f->size, f->size) && f->usage + k >= 1 && f->usage + k <= 8)
                    buttons |= 1 << (f->usage + k - 1);
            break;
        case HF_X:     ms = 1; dx += value(r, f, 0); break;
        case HF_Y:     ms = 1; dy += value(r, f, 0); break;
        case HF_WHEEL: ms = 1; wheel += value(r, f, 0); break;
        case HF_ABSX:  abs = 1; ax = scale(f, value(r, f, 0)); break;
        case HF_ABSY:  abs = 1; ay = scale(f, value(r, f, 0)); break;
        case HF_TIP:   abs = 1; tip = bits(r, f->bit, f->size) != 0; break;
        }
    }
    if (kb && !phantom)
        keys_changed(h, keys);
    if (abs) {                          /* a touch screen's or tablet's report */
        if (has_buttons)
            h->buttons = buttons;
        if (ax >= 0)
            h->ax = ax;
        if (ay >= 0)
            h->ay = ay;
        int32_t w = wheel;              /* (a tablet may have a relative wheel) */
        struct touch_event e = {
            .x = h->ax, .y = h->ay, .touch = (int8_t)tip,
            .wheel = (int8_t)(w > 127 ? 127 : w < -128 ? -128 : w),
            .buttons = h->buttons & 7,
        };
        evdev_touch(&e);
        return;
    }
    if (ms) {
        if (has_buttons)
            h->buttons = buttons;
        /* In PS/2's directions (mouse.h): HID's Y and wheel count the other way. */
        int32_t w = -wheel;
        struct mouse_event e = {
            .dx = clamp16(dx), .dy = clamp16(-dy),
            .wheel = (int8_t)(w > 127 ? 127 : w < -128 ? -128 : w),
            .buttons = h->buttons & 7,  /* left, right, middle */
        };
        mouse_report(&e);
    }
}
