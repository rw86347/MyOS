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
#define BIO_VEC_MAX  64         /* pieces in one readv */
#define BIO_SECTORS  256        /* sectors in one readv: 128 KiB */

/* A piece of physical memory for scatter-gather I/O: one disk command
   fills many page frames that are not next to each other. len is a
   multiple of 512; the piece is in the kernel's direct map and may span
   several frames that happen to be next to each other (drivers split it
   to suit their hardware). */
struct bio_vec {
    uint32_t phys;
    uint32_t len;
};

struct blockdev {
    char     name[16];          /* e.g. "hda", "hdb1" (Linux-style names) */
    uint32_t sectors;           /* size in 512-byte sectors */

    /* Read/write `count` sectors starting at `lba`. Return 0 on success,
       negative on error. Partitions leave these NULL and use parent+offset. */
    int (*read)(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf);
    /* Make written data permanent (a drive's own write cache). May be NULL. */
    int (*flush)(struct blockdev *dev);
    /* Read (write = 0) or write consecutive sectors from/to the pieces of v
       (at most BIO_VEC_MAX pieces, BIO_SECTORS sectors) with as few
       commands as possible. May be NULL: then it goes piece by piece. */
    int (*rwv)(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int n, int write);

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
int blockdev_readv(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int n);
int blockdev_writev(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int n);

/* For drivers: the physical pieces of a kernel buffer (pieces that touch
   are merged). The count, or -1 if it is unmapped or needs more than max. */
int blockdev_map(const void *buf, uint32_t bytes, struct bio_vec *v, int max);

/*
 * A request that doesn't wait ("bio", like Linux's): blockdev_submit hands
 * it to the disk and returns at once; the caller goes on (copying the last
 * read-ahead window to a program, say) while the disk works. When it is
 * finished, status becomes 0 or -1 and done(b) is called.
 *
 * done may run in an interrupt or in the disk's worker thread: it must be
 * short, must not sleep and must not allocate. The bio, and the pieces v
 * points at, belong to the disk until then.
 */
#define BIO_BUSY 1

struct bio {
    uint32_t lba;
    const struct bio_vec *v;
    int16_t  nv;
    uint8_t  write;
    volatile int8_t status;             /* BIO_BUSY, then 0 or -1 */
    void   (*done)(struct bio *b);      /* may be NULL (bio_wait instead) */
    void    *priv;                      /* the caller's */
    struct blockdev *dev;               /* the whole disk (set by submit) */
    struct bio *next;                   /* the disk's queue */
};

/* Start the request. 0: it is on its way (done will be called); -1: it was
   refused (bad range, read-only disk) and done will not be called. */
int blockdev_submit(struct blockdev *dev, struct bio *b);

/* Sleep until the request is finished; its status. */
int bio_wait(struct bio *b);

#endif
