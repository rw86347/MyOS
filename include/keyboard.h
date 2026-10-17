/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* Special keys, returned outside the ASCII range. */
#define KEY_F10 0x100
#define KEY_F11 0x101
#define KEY_F12 0x102

/* Start the PS/2 keyboard (IRQ 1). 0, or -1 if the PC has no PS/2
   controller (then there is no PS/2 mouse either). */
int keyboard_init(void);

/* A key went down (pressed = 1; again while held: it repeats) or up (0),
   by its Linux code (KEY_*: input-event-codes.h). From any keyboard's
   interrupt (PS/2 here, USB in usb_hid.c): to /dev/input/event0 and to
   the terminal. */
void keyboard_key(uint16_t key, int pressed);

/* Next typed character (ASCII, or a KEY_* value), or -1 if none waiting.
   (Only while no listener is set.) */
int keyboard_getc(void);

/* Send every key from now on to fn, as terminal bytes (tty.c). Called in
   the keyboard interrupt. */
void keyboard_set_listener(void (*fn)(const char *bytes, int n));

/* 0: keys stop going to the terminal; only /dev/input/event0 sees them
   (an X server's KDSKBMODE K_OFF). 1: back to normal. */
void keyboard_set_tty(int on);

#endif
