/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * test-hid.c — drivers/hid/hid.c on the Mac: a touchpad moves the pointer
 * like a mouse (`make test-hid`).
 *
 * QEMU has no USB touchpad, so the kernel's HID parser is compiled for the
 * host and fed a Windows precision touchpad's report descriptor (laid out
 * as Microsoft's sample is: a Touch Pad collection with two fingers, a
 * contact count and the pad's button, and the mouse collection such pads
 * also have) and its reports. What hid.c would send to the mouse and touch
 * paths is caught here instead.
 */
#include "../include/evdev.h"
#include "../include/hid.h"
#include "../include/mouse.h"

#include <stdio.h>
#include <string.h>

static struct mouse_event got[64];
static int ngot, ntouch;
static struct { uint16_t key; int pressed; } keys[16];
static int nkeys;
static uint32_t now_ms = 1000;
static int failures, passes;

void mouse_report(const struct mouse_event *e) { if (ngot < 64) got[ngot++] = *e; }
void evdev_touch(const struct touch_event *e) { (void)e; ntouch++; }
void keyboard_key(uint16_t key, int pressed)
{
    if (nkeys < 16)
        keys[nkeys].key = key, keys[nkeys++].pressed = pressed;
}
uint32_t timer_ms(void) { return now_ms; }

static void check(const char *what, int ok)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        passes++;
    else
        failures++;
}

/* Report 1: the touchpad. Per finger: confidence, tip (bits 0, 1), contact
   ID (a byte), X and Y (16 bits, 0-4095); two fingers, then the contact
   count and the button. Report 2: the mouse mode. */
static const uint8_t ptp[] = {
    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01,             /* Digitizer, Touch Pad, Application */
    0x85, 0x01,                                     /* Report ID 1 */
    /* finger 1 */
    0x09, 0x22, 0xA1, 0x02,                         /* Finger, Logical */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
    0x09, 0x47, 0x81, 0x02,                         /* Confidence */
    0x09, 0x42, 0x81, 0x02,                         /* Tip Switch */
    0x95, 0x06, 0x81, 0x03,                         /* padding */
    0x75, 0x08, 0x95, 0x01, 0x25, 0x0F,
    0x09, 0x51, 0x81, 0x02,                         /* Contact Identifier */
    0x05, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x95, 0x01,
    0x55, 0x0E, 0x65, 0x11, 0x35, 0x00, 0x46, 0xE8, 0x03,   /* units, physical size */
    0x09, 0x30, 0x81, 0x02,                         /* X */
    0x09, 0x31, 0x81, 0x02,                         /* Y */
    0xC0,
    /* finger 2 */
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
    0x09, 0x47, 0x81, 0x02, 0x09, 0x42, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03,
    0x75, 0x08, 0x95, 0x01, 0x25, 0x0F, 0x09, 0x51, 0x81, 0x02,
    0x05, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x95, 0x01,
    0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
    0xC0,
    0x05, 0x0D, 0x09, 0x54, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,   /* Contact Count */
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,   /* Button 1 */
    0x95, 0x07, 0x81, 0x03,
    0xC0,
    /* the mouse mode */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xC0, 0xC0,
};

/* One touchpad report: finger 1 (and a second finger, off unless asked). */
static void pad(struct hid *h, int tip, int conf, int id, int x, int y, int button, int tip2)
{
    uint8_t r[16] = { 1 };
    r[1] = (uint8_t)((conf ? 1 : 0) | (tip ? 2 : 0));
    r[2] = (uint8_t)id;
    r[3] = (uint8_t)x, r[4] = (uint8_t)(x >> 8);
    r[5] = (uint8_t)y, r[6] = (uint8_t)(y >> 8);
    r[7] = (uint8_t)(tip2 ? 3 : 0);
    r[8] = 1;
    r[9] = 0x20, r[10] = 0x0F, r[11] = 0x20, r[12] = 0x0F;     /* finger 2 far away */
    r[13] = (uint8_t)((tip ? 1 : 0) + (tip2 ? 1 : 0));
    r[14] = (uint8_t)button;
    hid_report(h, r, 15);
}

/* Hybrid mode: one finger per report, the count in the frame's first
   report and 0 in the rest. */
static void pad1b(struct hid *h, int tip, int id, int x, int y, int count, int button)
{
    uint8_t r[16] = { 1 };
    r[1] = (uint8_t)(1 | (tip ? 2 : 0));
    r[2] = (uint8_t)id;
    r[3] = (uint8_t)x, r[4] = (uint8_t)(x >> 8);
    r[5] = (uint8_t)y, r[6] = (uint8_t)(y >> 8);
    r[13] = (uint8_t)count;
    r[14] = (uint8_t)button;
    hid_report(h, r, 15);
}

static void pad1(struct hid *h, int tip, int id, int x, int y, int count)
{
    pad1b(h, tip, id, x, y, count, 0);
}

/* A hybrid-mode frame of three fingers side by side, 600 units apart. */
static void frame3(struct hid *h, int x, int y)
{
    pad1(h, 1, 1, x, y, 3);
    pad1(h, 1, 2, x + 600, y, 0);
    pad1(h, 1, 3, x + 1200, y, 0);
}

/* Three fingers land at (x, y) and slide steps * (dx, dy). */
static void slide3(struct hid *h, int x, int y, int dx, int dy, int steps)
{
    frame3(h, x, y);
    for (int i = 1; i <= steps; i++) {
        now_ms += 8;
        frame3(h, x + dx * i, y + dy * i);
    }
}

static void lift3(struct hid *h, int x, int y)
{
    pad1(h, 0, 1, x, y, 3);
    pad1(h, 0, 2, x + 600, y, 0);
    pad1(h, 0, 3, x + 1200, y, 0);
}

/* One key, pressed and let go. */
static int one_key(uint16_t key)
{
    return nkeys == 2 && keys[0].key == key && keys[0].pressed && keys[1].key == key &&
           !keys[1].pressed;
}

static int all_held(void) { for (int i = 0; i < ngot; i++) if (got[i].buttons != 1) return 0; return 1; }

static int total_dx(void) { int t = 0; for (int i = 0; i < ngot; i++) t += got[i].dx; return t; }
static int total_dy(void) { int t = 0; for (int i = 0; i < ngot; i++) t += got[i].dy; return t; }
static int total_wheel(void) { int t = 0; for (int i = 0; i < ngot; i++) t += got[i].wheel; return t; }

int main(void)
{
    static struct hid h;
    printf("hid.c, a precision touchpad:\n");
    check("the descriptor is read", hid_parse(&h, ptp, sizeof ptp) == 0);
    check("it is a touchpad, not a touch screen", h.touchpad && !h.touch);

    /* A finger lands: no jump to where it is. */
    ngot = 0;
    pad(&h, 1, 1, 3, 1000, 1000, 0, 0);
    check("a finger landing doesn't move the pointer", total_dx() == 0 && total_dy() == 0);
    /* It moves right 410 units (a tenth of the pad), then down. */
    for (int i = 1; i <= 10; i++) {
        now_ms += 8;
        pad(&h, 1, 1, 3, 1000 + 41 * i, 1000, 0, 0);
    }
    check("moving right is relative movement to the right (about 60 counts)",
          total_dx() >= 59 && total_dx() <= 61 && total_dy() == 0);
    check("nothing went to the touch screen path", ntouch == 0);
    ngot = 0;
    now_ms += 8;
    pad(&h, 1, 1, 3, 1410, 1200, 0, 0);
    check("moving down is negative dy (PS/2's up is positive)", total_dy() < 0 && total_dx() == 0);
    /* Slow: 3 units at a time is under one count each, but adds up. */
    ngot = 0;
    for (int i = 1; i <= 20; i++)
        pad(&h, 1, 1, 3, 1410 + 3 * i, 1200, 0, 0);
    check("a slow finger still moves (the remainders add up)", total_dx() >= 8 && total_dx() <= 9);
    /* A second finger's report in the first slot (hybrid mode) is ignored. */
    ngot = 0;
    pad(&h, 1, 1, 5, 3000, 3000, 0, 0);
    check("another contact's report doesn't move the pointer", total_dx() == 0 && total_dy() == 0);
    /* Lifted after moving: no click. */
    ngot = 0;
    now_ms += 300;
    pad(&h, 0, 1, 3, 1470, 1200, 0, 0);
    check("lifting after a move is not a click", ngot == 0);

    /* A tap: down and up within 180 ms, without moving. */
    ngot = 0;
    now_ms += 500;
    pad(&h, 1, 1, 4, 2000, 2000, 0, 0);
    now_ms += 60;
    pad(&h, 0, 1, 4, 2000, 2000, 0, 0);
    check("a tap is a left click (press, then release)",
          ngot == 2 && got[0].buttons == 1 && got[1].buttons == 0);
    /* A long touch is not. */
    ngot = 0;
    now_ms += 500;
    pad(&h, 1, 1, 4, 2000, 2000, 0, 0);
    now_ms += 400;
    pad(&h, 0, 1, 4, 2000, 2000, 0, 0);
    check("a long touch is not a click", ngot == 0);

    /* The pad's own button. */
    ngot = 0;
    now_ms += 500;
    pad(&h, 1, 1, 6, 2000, 2000, 0, 0);
    now_ms += 30;
    pad(&h, 1, 1, 6, 2000, 2000, 1, 0);
    now_ms += 30;
    pad(&h, 1, 1, 6, 2000, 2000, 0, 0);
    pad(&h, 0, 1, 6, 2000, 2000, 0, 0);
    check("the pad's button clicks, once (not a tap as well)",
          ngot == 2 && got[0].buttons == 1 && got[1].buttons == 0);

    /* A palm: the pad says it isn't a finger. */
    ngot = 0;
    now_ms += 500;
    pad(&h, 1, 0, 7, 500, 500, 0, 0);
    pad(&h, 1, 0, 7, 900, 900, 0, 0);
    now_ms += 50;
    pad(&h, 0, 0, 7, 900, 900, 0, 0);
    check("a palm neither moves the pointer nor taps", ngot == 0);

    /* Two fingers move up a fifth of the pad (820 units): the page follows
       them (natural scrolling), so the wheel turns towards the user (+),
       about 820 * 600 / 4095 / 24 = 5 clicks, and the pointer stays. */
    ngot = 0;
    now_ms += 500;
    pad(&h, 1, 1, 8, 2000, 3000, 0, 1);
    for (int i = 1; i <= 20; i++) {
        now_ms += 8;
        pad(&h, 1, 1, 8, 2000, 3000 - 41 * i, 0, 1);
    }
    check("two fingers moving up scroll down, about 5 clicks",
          total_wheel() == 5 && total_dx() == 0 && total_dy() == 0);
    ngot = 0;
    for (int i = 1; i <= 20; i++) {
        now_ms += 8;
        pad(&h, 1, 1, 8, 2000, 2180 + 41 * i, 0, 1);
    }
    check("two fingers moving down scroll up", total_wheel() == -5 && total_dy() == 0);
    /* One lifts: the other moves the pointer again, with no jump. */
    ngot = 0;
    pad(&h, 1, 1, 8, 2000, 3000, 0, 0);
    now_ms += 8;
    pad(&h, 1, 1, 8, 2041, 3000, 0, 0);
    check("back to one finger: it points again", total_dx() > 0 && total_wheel() == 0);
    ngot = 0;
    now_ms += 50;
    pad(&h, 0, 1, 8, 2041, 3000, 0, 0);
    check("lifting after a scroll is not a click", ngot == 0);

    /* A quick two-finger tap: the right button. */
    ngot = 0;
    now_ms += 500;
    pad(&h, 1, 1, 9, 2000, 2000, 0, 1);
    now_ms += 60;
    pad(&h, 0, 1, 9, 2000, 2000, 0, 0);
    check("a two-finger tap is a right click (press, then release)",
          ngot == 2 && got[0].buttons == 2 && got[1].buttons == 0);

    /* Hybrid mode: one finger per report, the count only in the first. */
    ngot = 0;
    now_ms += 500;
    pad1(&h, 1, 10, 2000, 3000, 2);
    pad1(&h, 1, 11, 2600, 3000, 0);
    for (int i = 1; i <= 20; i++) {
        now_ms += 8;
        pad1(&h, 1, 10, 2000, 3000 - 41 * i, 2);
        pad1(&h, 1, 11, 2600, 3000 - 41 * i, 0);
    }
    check("hybrid mode: two fingers scroll too", total_wheel() == 5 && total_dx() == 0 && total_dy() == 0);
    pad1(&h, 0, 10, 2000, 2180, 2);
    pad1(&h, 0, 11, 2600, 2180, 0);

    /* Dragging as on a Mac (hybrid mode, the P2 Max's): a thumb rests in
       the bottom-left corner, another finger lands, the thumb presses the
       pad (wobbling a little as it gives way), the other finger moves
       right a tenth of the pad. The pointer follows the moving finger,
       about 60 counts less the 12 before a drag starts, the button held
       all the while; nothing scrolls. */
    ngot = 0;
    now_ms += 500;
    pad1b(&h, 1, 1, 200, 3900, 1, 0);
    now_ms += 300;
    pad1b(&h, 1, 1, 200, 3900, 2, 0);
    pad1b(&h, 1, 2, 2000, 2000, 0, 0);
    now_ms += 8;
    pad1b(&h, 1, 1, 200, 3900, 2, 1);
    pad1b(&h, 1, 2, 2000, 2000, 0, 1);
    for (int i = 1; i <= 10; i++) {
        now_ms += 8;
        pad1b(&h, 1, 1, 200 + (i & 1) * 20, 3900 - (i & 1) * 20, 2, 1);   /* the thumb's wobble */
        pad1b(&h, 1, 2, 2000 + 41 * i, 2000, 0, 1);
    }
    check("thumb holds the click, another finger drags",
          total_dx() >= 45 && total_dx() <= 50 && total_dy() == 0 && total_wheel() == 0 &&
          all_held());
    ngot = 0;
    now_ms += 8;
    pad1b(&h, 1, 1, 200, 3900, 2, 0);
    pad1b(&h, 1, 2, 2410, 2000, 0, 0);
    pad1b(&h, 0, 1, 200, 3900, 2, 0);
    pad1b(&h, 0, 2, 2410, 2000, 0, 0);
    check("letting go releases the button, and is no tap",
          ngot >= 1 && got[0].buttons == 0 && total_wheel() == 0 &&
          !(ngot > 1 && got[ngot - 1].buttons));
    /* One finger pressing and moving drags too. */
    ngot = 0;
    now_ms += 500;
    pad1b(&h, 1, 3, 1000, 1000, 1, 0);
    now_ms += 300;
    pad1b(&h, 1, 3, 1000, 1000, 1, 1);
    for (int i = 1; i <= 10; i++) {
        now_ms += 8;
        pad1b(&h, 1, 3, 1000, 1000 + 41 * i, 1, 1);
    }
    check("one finger pressing and moving drags", total_dy() < -40 && all_held());
    pad1b(&h, 0, 3, 1000, 1410, 1, 0);

    /* Two-finger scrolling off (Settings): two fingers point instead. */
    hid_two_finger_scroll = 0;
    ngot = 0;
    now_ms += 500;
    pad1(&h, 1, 4, 2000, 3000, 2);
    pad1(&h, 1, 5, 2600, 3000, 0);
    for (int i = 1; i <= 10; i++) {
        now_ms += 8;
        pad1(&h, 1, 4, 2000, 3000 - 41 * i, 2);
        pad1(&h, 1, 5, 2600, 3000 - 41 * i, 0);
    }
    check("scrolling off: two fingers move the pointer", total_wheel() == 0 && total_dy() > 50);
    pad1(&h, 0, 4, 2000, 2590, 2);
    pad1(&h, 0, 5, 2600, 2590, 0);
    hid_two_finger_scroll = 1;

    /* Three fingers slide left a quarter of the pad (1025 units, about 150
       counts, past the 120 of a swipe): one swipe-left key; the pointer and
       the wheel stay still. Sliding on sends nothing more. */
    ngot = nkeys = 0;
    now_ms += 500;
    slide3(&h, 2000, 2000, -41, 0, 25);
    check("three fingers sliding left: one swipe-left key, pressed and let go",
          one_key(HID_SWIPE_LEFT) && ngot == 0);
    nkeys = 0;
    for (int i = 1; i <= 20; i++) {
        now_ms += 8;
        frame3(&h, 975 - 41 * i, 2000);
    }
    check("sliding on: no second key, still no pointer", nkeys == 0 && ngot == 0);
    /* They lift one by one, the last two still sliding a little. */
    pad1(&h, 0, 1, 155, 2000, 3);
    pad1(&h, 1, 2, 755, 2000, 0);
    pad1(&h, 1, 3, 1355, 2000, 0);
    for (int i = 1; i <= 5; i++) {
        now_ms += 8;
        pad1(&h, 1, 2, 755 - 41 * i, 2000, 2);
        pad1(&h, 1, 3, 1355 - 41 * i, 2000, 0);
    }
    pad1(&h, 0, 2, 550, 2000, 2);
    pad1(&h, 1, 3, 1150, 2000, 0);
    for (int i = 1; i <= 5; i++) {
        now_ms += 8;
        pad1(&h, 1, 3, 1150 - 41 * i, 2000, 1);
    }
    pad1(&h, 0, 3, 945, 2000, 1);
    check("the fingers lifting one by one move nothing and click nothing", ngot == 0 && nkeys == 0);
    /* The next touch is an ordinary one. */
    now_ms += 500;
    pad1(&h, 1, 4, 2000, 2000, 1);
    for (int i = 1; i <= 10; i++) {
        now_ms += 8;
        pad1(&h, 1, 4, 2000 + 41 * i, 2000, 1);
    }
    check("then one finger points again", total_dx() >= 59 && nkeys == 0);
    pad1(&h, 0, 4, 2410, 2000, 1);

    /* The other three ways. */
    static const struct { int x, y, dx, dy; uint16_t key; const char *what; } ways[] = {
        { 500, 2000, 41, 0, HID_SWIPE_RIGHT, "three fingers sliding right: swipe-right" },
        { 1000, 3000, 0, -41, HID_SWIPE_UP, "three fingers sliding up: swipe-up" },
        { 1000, 1000, 0, 41, HID_SWIPE_DOWN, "three fingers sliding down: swipe-down" },
    };
    for (unsigned w = 0; w < sizeof ways / sizeof ways[0]; w++) {
        ngot = nkeys = 0;
        now_ms += 500;
        slide3(&h, ways[w].x, ways[w].y, ways[w].dx, ways[w].dy, 25);
        check(ways[w].what, one_key(ways[w].key) && ngot == 0);
        lift3(&h, ways[w].x + ways[w].dx * 25, ways[w].y + ways[w].dy * 25);
    }
    /* A short slide, a tenth of the pad: no key, and nothing else either. */
    ngot = nkeys = 0;
    now_ms += 500;
    slide3(&h, 2000, 2000, -41, 0, 10);
    check("a short three-finger slide sends nothing", nkeys == 0 && ngot == 0);
    lift3(&h, 1590, 2000);

    /* The mouse mode still works as a mouse. */
    ngot = 0;
    uint8_t m[4] = { 2, 0, 5, (uint8_t)-3 };
    hid_report(&h, m, 4);
    check("its mouse-mode report is a plain mouse report",
          ngot == 1 && got[0].dx == 5 && got[0].dy == 3);

    printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
