#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Special keys, returned outside the ASCII range. */
#define KEY_F11 0x100
#define KEY_F12 0x101

void keyboard_init(void);

/* Next typed character (ASCII, or a KEY_* value), or -1 if none waiting. */
int keyboard_getc(void);

#endif
