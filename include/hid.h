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
    uint16_t bytes;                     /* the longest input report, ID included */
    uint8_t  buttons;                   /* mouse buttons held */
    int32_t  ax, ay;                    /* an absolute pointer's place, 0..EVDEV_ABS_MAX */
    uint8_t  keys[32];                  /* keyboard usages held, a bitmap */
};

/* Read a report descriptor: 0 if the device has keys or a pointer. */
int hid_parse(struct hid *h, const uint8_t *desc, int len);

/* One input report of n bytes: the key and mouse events in it (called in
   the interrupt that brought it). */
void hid_report(struct hid *h, const uint8_t *r, int n);

#endif
