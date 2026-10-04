#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Special keys, returned outside the ASCII range. */
#define KEY_F10 0x100
#define KEY_F11 0x101
#define KEY_F12 0x102

void keyboard_init(void);

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
