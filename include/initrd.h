#ifndef INITRD_H
#define INITRD_H

#include <stdint.h>

/* The initramfs: a cpio archive of files that the bootloader loaded into
   RAM next to the kernel. Step 7 turns it into the root file system. */

/* Find the initramfs (from the boot info) and check it arrived intact. */
void initrd_init(void);

/* Where it is in (virtual) memory and how big; NULL/0 if there is none. */
const uint8_t *initrd_data(void);
uint32_t initrd_size(void);

#endif
