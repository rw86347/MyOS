#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* One movement of the PS/2 mouse (drivers/mouse.c), as the mouse reports
   it: dy > 0 is UP (screen coordinates go down). */
struct mouse_event {
    int16_t dx, dy;
    int8_t  wheel;                  /* +1 / -1 per notch (0 without a wheel) */
    uint8_t buttons;                /* bit 0 left, 1 right, 2 middle: held now */
};

/* Find and start the mouse (call with interrupts still off). 0, or -1 if
   there is none. */
int mouse_init(void);

/* The oldest movement not yet taken: 0, or -1 if there is none. */
int mouse_get(struct mouse_event *e);

/* Called (in the interrupt) after each new movement: wake whoever waits. */
void mouse_set_listener(void (*fn)(void));

#endif
