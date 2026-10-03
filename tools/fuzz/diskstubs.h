/* diskstubs.h — the host side of the disk fuzz target. */
#ifndef FUZZ_DISKSTUBS_H
#define FUZZ_DISKSTUBS_H

#include <stdint.h>

#include <blockdev.h>

/* Put an image behind a block device and bring up the caches. Called once per
   process: each case runs in a fresh fork, so there is nothing to undo. */
struct blockdev *disk_setup(const uint8_t *image, uint32_t bytes);

/* The image as the device sees it, after padding to a whole number of
   sectors. The target uses this to know how big the "disk" is. */
uint32_t disk_sectors(void);

/* Every byte kmalloc hands out is filled with this first. Nothing on a disk
   has a reason to contain it, so a byte of it appearing in something the
   kernel reports -- a symlink target, a filename -- means uninitialized
   kernel heap was disclosed. That is a bug sanitizers cannot see, because the
   memory is validly allocated; it is just never written. */
#define HEAP_POISON 0xA5

/* How many bytes of a buffer look like heap poison, counting from the start.
   Used by the checks to catch a disclosure of memory nobody filled in. */
uint32_t poison_run(const void *p, uint32_t n);

/* Give the child a shared coverage map to mark. See diskstubs.c. */
void cov_use(uint8_t *map, uint32_t n);

#endif
