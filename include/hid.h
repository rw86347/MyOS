/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef HID_H
#define HID_H

#include <stdint.h>

/*
 * hid.h — HID reports (drivers/hid/hid.c): keyboards and mice, whatever
 * wire they come over (USB: usb_hid.c; I2C later: docs/TOUCHPAD-PLAN.md).
 */

#define HID_FIELDS 24                   /* the fields kept per device */
#define PAD_CONTACTS 8                  /* a touchpad's fingers followed, by contact ID */

/* Three fingers sliding together on a touchpad: one of these keys, pressed
   and let go, once per touch, for the desktop to act on (myos-menu). Linux
   codes of F14-F17, which no keyboard here has; X's keycode is the code
   + 8. They name the way the fingers went, not what it does. */
#define HID_SWIPE_LEFT  184
#define HID_SWIPE_RIGHT 185
#define HID_SWIPE_UP    186
#define HID_SWIPE_DOWN  187

/* One touchpad finger: where it was, what is left of a count, where it
   was when the pad was pressed, and whether it drags (hid.c, touchpad()). */
struct hid_contact {
    int32_t x, y, fx, fy, px, py;
    uint8_t down, drags;
};

struct hid_field {
    uint8_t  id;                        /* the report it is in (0: no IDs) */
    uint8_t  kind;                      /* HF_* (hid.c) */
    uint8_t  size;                      /* bits per value */
    uint16_t count;                     /* values */
    uint16_t bit;                       /* where the first starts (after the ID byte) */
    uint16_t usage;                     /* the first value's usage */
    int32_t  lmin, lmax;                /* the values it can take */
};

struct hid {
    struct hid_field f[HID_FIELDS];
    uint8_t  nf;
    uint8_t  ids;                       /* each report starts with its ID byte */
    uint8_t  keyboard, mouse, touch;    /* what it has (touch: an absolute pointer) */
    uint8_t  touchpad;                  /* a touchpad: fingers' places become movement */
    uint16_t bytes;                     /* the longest input report, ID included */
    uint8_t  buttons;                   /* mouse buttons held */
    int32_t  ax, ay;                    /* an absolute pointer's place, 0..EVDEV_ABS_MAX */
    uint8_t  keys[32];                  /* keyboard usages held, a bitmap */
    /* A touchpad (hid.c, touchpad()): each finger, and the one followed
       (it points, scrolls and taps) and whether it can still be a tap. */
    struct hid_contact pc[PAD_CONTACTS];
    uint8_t  pad_down, pad_sent;        /* a finger is followed; the buttons last sent */
    uint8_t  pad_tap;                   /* still a tap: short, still, no click */
    uint8_t  pad_fingers;               /* fingers down, as the pad last said (Contact Count) */
    uint8_t  pad_two;                   /* two fingers were down during this touch */
    uint8_t  pad_held;                  /* the pad's button was held at the last report */
    uint8_t  pad_swipe;                 /* three fingers were down: a swipe until all are up */
    uint8_t  pad_swiped;                /* ...and its key went */
    int32_t  pad_sx, pad_sy;            /* ...the counts it has gone so far */
    int32_t  pad_id;                    /* its contact ID (-1: the pad sends none) */
    int32_t  pad_moved;                 /* counts it has moved since it touched */
    int32_t  pad_fw;                    /* scroll counts short of a wheel click */
    uint32_t pad_t0;                    /* when it touched (timer_ms) */
};

/* Two fingers scroll (1) or point (0): Settings > Mouse & Keyboard, through
   /sys/module/hid/parameters/two_finger_scroll. */
extern int hid_two_finger_scroll;

/* Read a report descriptor: 0 if the device has keys or a pointer. */
int hid_parse(struct hid *h, const uint8_t *desc, int len);

/* One input report of n bytes: the key and mouse events in it (called in
   the interrupt that brought it). */
void hid_report(struct hid *h, const uint8_t *r, int n);

#endif
