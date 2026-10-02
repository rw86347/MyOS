/*
 * blockdev.h — the block device layer
 *
 * A "block device" is anything that stores data in fixed-size sectors
 * (512 bytes here) addressed by number: an IDE disk, a SATA disk, a USB
 * stick, a partition on one of those, a RAM disk...
 *
 * File systems only talk to this interface, never to the hardware, so one
 * FAT32 driver works on every kind of disk. (Linux has the same split: the
 * "block layer" sits between file systems and disk drivers.)
 *
 * A partition is just a block device whose sector 0 is an offset into its
 * parent device — see fs/partition.c.
 */
#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

#define SECTOR_SIZE 512
#define BLOCKDEV_MAX 16

struct blockdev {
    char     name[16];          /* e.g. "hda", "hdb1" (Linux-style names) */
    uint32_t sectors;           /* size in 512-byte sectors */

    /* Read/write `count` sectors starting at `lba`. Return 0 on success,
       negative on error. Partitions leave these NULL and use parent+offset. */
    int (*read)(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf);
    /* Make written data permanent (a drive's own write cache). May be NULL. */
    int (*flush)(struct blockdev *dev);

    struct blockdev *parent;    /* for partitions: the whole disk */
    uint32_t offset;            /* for partitions: first sector on the parent */
    void    *priv;              /* driver's private data */
};

/* Add a device to the global list. Returns it, or NULL if the list is full. */
struct blockdev *blockdev_register(const struct blockdev *dev);

int  blockdev_count(void);
struct blockdev *blockdev_get(int index);

/* Read/write sectors on any device (handles partitions, checks bounds). */
int blockdev_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf);
int blockdev_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf);
int blockdev_flush(struct blockdev *dev);

#endif
