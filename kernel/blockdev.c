/*
 * blockdev.c — global list of block devices + read/write dispatch.
 *
 * Devices live in a small fixed-size array: a PC has only a few disks.
 */
#include <blockdev.h>
#include <memlayout.h>
#include <paging.h>
#include <stddef.h>

static struct blockdev devices[BLOCKDEV_MAX];
static int ndevices;

struct blockdev *blockdev_register(const struct blockdev *dev)
{
    if (ndevices == BLOCKDEV_MAX)
        return NULL;
    devices[ndevices] = *dev;
    return &devices[ndevices++];
}

int blockdev_count(void)
{
    return ndevices;
}

struct blockdev *blockdev_get(int index)
{
    return (index >= 0 && index < ndevices) ? &devices[index] : NULL;
}

/* Is [lba, lba+count) inside the device? Written so it can't overflow. */
static int in_range(struct blockdev *dev, uint32_t lba, uint32_t count)
{
    return count <= dev->sectors && lba <= dev->sectors - count;
}

int blockdev_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    if (!in_range(dev, lba, count))
        return -1;
    if (dev->parent)    /* partition: shift into the parent disk */
        return blockdev_read(dev->parent, dev->offset + lba, count, buf);
    return dev->read(dev, lba, count, buf);
}

int blockdev_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    if (!in_range(dev, lba, count))
        return -1;
    if (dev->parent)
        return blockdev_write(dev->parent, dev->offset + lba, count, buf);
    if (!dev->write)
        return -1;      /* read-only device */
    return dev->write(dev, lba, count, buf);
}

int blockdev_map(const void *buf, uint32_t bytes, struct bio_vec *v, int max)
{
    uintptr_t va = (uintptr_t)buf;
    int n = 0;
    while (bytes) {
        uint32_t piece = PAGE_SIZE - (va & (PAGE_SIZE - 1)), pa;
        if (piece > bytes)
            piece = bytes;
        if (va >= KERNEL_VBASE && va - KERNEL_VBASE < DIRECTMAP_LIMIT)
            pa = V2P(va);
        else if ((pa = vmm_translate(va & ~(PAGE_SIZE - 1))) != 0)
            pa |= va & (PAGE_SIZE - 1);
        if (!pa)
            return -1;
        if (n && v[n - 1].phys + v[n - 1].len == pa)
            v[n - 1].len += piece;
        else if (n == max)
            return -1;
        else
            v[n++] = (struct bio_vec){ pa, piece };
        va += piece;
        bytes -= piece;
    }
    return n;
}

static int rwv(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int n, int write)
{
    uint32_t count = 0;
    for (int k = 0; k < n; k++)
        count += v[k].len / SECTOR_SIZE;
    if (n < 1 || n > BIO_VEC_MAX || count > BIO_SECTORS || !in_range(dev, lba, count))
        return -1;
    while (dev->parent) {
        lba += dev->offset;
        dev = dev->parent;
    }
    if (write && !dev->write)
        return -1;                              /* read-only device */
    if (dev->rwv)
        return dev->rwv(dev, lba, v, n, write);
    for (int k = 0; k < n; k++) {               /* no scatter-gather: piece by piece */
        uint32_t c = v[k].len / SECTOR_SIZE;
        if ((write ? dev->write(dev, lba, c, P2V(v[k].phys))
                   : dev->read(dev, lba, c, P2V(v[k].phys))) < 0)
            return -1;
        lba += c;
    }
    return 0;
}

int blockdev_readv(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int n)
{
    return rwv(dev, lba, v, n, 0);
}

int blockdev_writev(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int n)
{
    return rwv(dev, lba, v, n, 1);
}

int blockdev_flush(struct blockdev *dev)
{
    while (dev->parent)
        dev = dev->parent;          /* the drive the partition is on */
    return dev->flush ? dev->flush(dev) : 0;
}
