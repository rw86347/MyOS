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

void evdev_key(uint16_t code, int pressed);         /* a key, Linux KEY_* code */
void evdev_mouse(const struct mouse_event *e);      /* one PS/2 movement */

/* For device nodes 13:64 (event0, the keyboard) and 13:65 (event1, the
   mouse); NULL for any other minor. */
const struct file_ops *evdev_ops(uint32_t minor);

#endif
