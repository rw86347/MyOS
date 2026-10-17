/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef EVDEV_H
#define EVDEV_H

#include <stdint.h>

/*
 * /dev/input/event* (drivers/evdev.c): the keyboard and the mouse as
 * Linux "input event" devices, the way X servers and Wayland read them.
 * The drivers call these from their interrupt handlers.
 */
struct mouse_event;
struct file_ops;

/* An absolute pointer (a touch screen, a tablet, QEMU's usb-tablet): where
   it points, 0..EVDEV_ABS_MAX across the whole screen each way. */
#define EVDEV_ABS_MAX 32767
struct touch_event {
    int32_t x, y;
    int8_t  touch;                      /* a finger or pen is down: 1/0; -1: can't tell */
    int8_t  wheel;                      /* notches, away from the user > 0 */
    uint8_t buttons;                    /* bit 0 left, 1 right, 2 middle */
};

void evdev_key(uint16_t code, int pressed);         /* a key, Linux KEY_* code */
void evdev_mouse(const struct mouse_event *e);      /* one PS/2 movement */
void evdev_touch(const struct touch_event *e);      /* event2 */

/* For device nodes 13:64 (event0, the keyboard), 13:65 (event1, the
   mouse) and 13:66 (event2, touch screens and tablets); NULL for any
   other minor. */
const struct file_ops *evdev_ops(uint32_t minor);

#endif
