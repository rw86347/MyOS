#ifndef HPET_H
#define HPET_H

#include <stdint.h>

/* Make the HPET drive IRQ 0 at `hz` in place of the PIT (drivers/hpet.c).
   0, or -1 if there is no usable HPET. */
int hpet_legacy_start(uint32_t hz);

#endif
