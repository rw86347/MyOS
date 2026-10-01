/*
 * blockdev.c — global list of block devices + read/write dispatch.
 *
 * No kmalloc yet (roadmap step 3), so devices live in a fixed-size array.
 */
#include <blockdev.h>
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
