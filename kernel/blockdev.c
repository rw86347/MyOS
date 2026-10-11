/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * blockdev.c — global list of block devices + read/write dispatch.
 *
 * Devices live in a small fixed-size array: a PC has only a few disks.
 */
#include <blockdev.h>
#include <memlayout.h>
#include <paging.h>
#include <interrupts.h>
#include <stddef.h>
#include <thread.h>

static struct blockdev devices[BLOCKDEV_MAX];
static int ndevices;

struct blockdev *blockdev_register(const struct blockdev *dev)
{
    uint32_t flags = irq_save();        /* the usb thread adds disks too */
    struct blockdev *d = NULL;
    if (ndevices < BLOCKDEV_MAX) {
        devices[ndevices] = *dev;
        d = &devices[ndevices++];
    }
    irq_restore(flags);
    if (d) {
        sysfs_block_add(d);             /* /sys/block/<name> (fs/sysfs.c) */
        dev_block_add(d);               /* /dev/<name> (fs/dev.c) */
    }
    return d;
}

void blockdev_changed(struct blockdev *d)
{
    sysfs_block_remove(d);
    dev_block_remove(d);
    if (d->sectors) {
        sysfs_block_add(d);
        dev_block_add(d);
    }
}

struct blockdev *blockdev_by_devnum(unsigned major, unsigned minor)
{
    for (int k = 0; k < ndevices; k++) {    /* a few disks: at open and mount only */
        unsigned ma, mi;
        blockdev_devnum(&devices[k], &ma, &mi);
        if (ma == major && mi == minor && devices[k].sectors)
            return &devices[k];
    }
    return NULL;
}

void blockdev_devnum(const struct blockdev *d, unsigned *major, unsigned *minor)
{
    const struct blockdev *disk = d->parent ? d->parent : d;
    unsigned part = d->parent ? d->partno : 0;
    const char *n = disk->name;
    if (n[0] == 's' && n[1] == 'd') {
        *major = 8;
        *minor = (unsigned)(n[2] - 'a') * 16 + part;
    } else if (n[0] == 'h' && n[1] == 'd') {
        unsigned unit = (unsigned)(n[2] - 'a');
        *major = unit < 2 ? 3 : 22;
        *minor = (unit & 1) * 64 + part;
    } else if (n[0] == 'v' && n[1] == 'd') {
        *major = 252;
        *minor = (unsigned)(n[2] - 'a') * 16 + part;
    } else {                            /* nvme0n1: Linux numbers these in order */
        *major = 259;
        *minor = 0;
        for (int i = 0; i < blockdev_count(); i++)
            if (blockdev_get(i) == d)
                *minor = (unsigned)i;
    }
}

void blockdev_sd_name(char name[16])
{
    static int next;
    uint32_t flags = irq_save();
    int n = next++;
    irq_restore(flags);
    name[0] = 's';
    name[1] = 'd';
    name[2] = (char)('a' + n % 26);
    name[3] = 0;
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
        if (va >= DIRECTMAP_BASE && va - DIRECTMAP_BASE < DIRECTMAP_LIMIT)
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

/* --- requests that don't wait ---------------------------------------------- */

/*
 * Each disk gets a worker thread the first time a request is submitted to
 * it. The worker runs queued requests one after another through the
 * driver's ordinary rwv (which sleeps on the interrupt), so the submitter
 * never waits. Costs one thread switch per request; a driver that can take
 * several commands at once will later queue them on the hardware itself.
 */
static struct {
    struct bio *head, *tail;
    struct wait_queue wait;             /* the worker sleeps here */
    uint8_t started;
} queue[BLOCKDEV_MAX];

static struct wait_queue bio_done;      /* bio_wait sleeps here */

static void finish(struct bio *b, int status)
{
    b->status = (int8_t)status;
    if (b->done)
        b->done(b);
    wq_wake_all(&bio_done);
}

static void worker(void *arg)
{
    int d = (int)(uintptr_t)arg;
    uint32_t flags = irq_save();
    for (;;) {
        while (!queue[d].head)
            wq_sleep(&queue[d].wait);
        struct bio *b = queue[d].head;
        queue[d].head = b->next;
        if (!queue[d].head)
            queue[d].tail = NULL;
        finish(b, rwv(b->dev, b->lba, b->v, b->nv, b->write) < 0 ? -1 : 0);
    }
    irq_restore(flags);
}

int blockdev_submit(struct blockdev *dev, struct bio *b)
{
    uint32_t count = 0;
    for (int k = 0; k < b->nv; k++)
        count += b->v[k].len / SECTOR_SIZE;
    if (b->nv < 1 || b->nv > BIO_VEC_MAX || count > BIO_SECTORS || !in_range(dev, b->lba, count))
        return -1;
    while (dev->parent) {
        b->lba += dev->offset;
        dev = dev->parent;
    }
    if (b->write && !dev->write)
        return -1;
    int d = dev - devices;
    b->dev = dev;
    b->status = BIO_BUSY;
    b->next = NULL;
    uint32_t flags = irq_save();
    if (!queue[d].started) {
        queue[d].started = 1;
        if (!thread_create("blk", worker, (void *)(uintptr_t)d)) {
            queue[d].started = 0;
            irq_restore(flags);
            return -1;
        }
    }
    if (queue[d].tail)
        queue[d].tail->next = b;
    else
        queue[d].head = b;
    queue[d].tail = b;
    wq_wake_all(&queue[d].wait);
    irq_restore(flags);
    return 0;
}

int bio_wait(struct bio *b)
{
    uint32_t flags = irq_save();
    while (b->status == BIO_BUSY)
        wq_sleep(&bio_done);
    irq_restore(flags);
    return b->status;
}
