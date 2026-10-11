/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * virtio_blk.c — the virtio block device (QEMU's and KVM's fast disk)
 *
 * A virtual machine doesn't have to pretend to be an IDE disk: "virtio" is
 * a device made for VMs. The driver and the device share a ring of request
 * descriptors in RAM (a "virtqueue"); a request is a chain of them:
 *
 *   [header: read/write/flush, sector] -> [data] -> [data] ... -> [status]
 *
 * The driver puts the chain's first descriptor in the "available" ring and
 * writes the queue number to the notify port (one I/O write). The device
 * moves the data by DMA, writes the status byte, puts the chain in the
 * "used" ring and interrupts. So a 128 KiB read is ~4 port accesses and one
 * interrupt — fewer than IDE's dozen — and the data pieces are the page
 * cache's frames themselves.
 *
 * This is the "legacy" PCI interface (virtio 0.9.5): registers in I/O port
 * BAR 0. QEMU's virtio-blk-pci offers it next to the modern one.
 * One request at a time per disk, like the IDE driver: the thread that
 * asked sleeps until the interrupt; others queue up behind it.
 */
#include <blockdev.h>
#include <interrupts.h>
#include <io.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pci.h>
#include <string.h>
#include <thread.h>
#include <timer.h>
#include <virtio_blk.h>

#define VIRTIO_VENDOR   0x1AF4
#define VIRTIO_BLK_DEV  0x1001          /* legacy (transitional) block device */

/* Legacy registers, offsets from BAR 0 (no MSI-X: config starts at 0x14). */
#define VIO_DEV_FEATURES   0x00
#define VIO_GUEST_FEATURES 0x04
#define VIO_QUEUE_PFN      0x08
#define VIO_QUEUE_SIZE     0x0C
#define VIO_QUEUE_SEL      0x0E
#define VIO_QUEUE_NOTIFY   0x10
#define VIO_STATUS         0x12
#define VIO_ISR            0x13
#define VIO_CONFIG         0x14         /* virtio-blk: capacity, 64-bit sectors */

#define ST_ACK        1
#define ST_DRIVER     2
#define ST_DRIVER_OK  4

#define F_RO     (1u << 5)              /* the disk is read-only */
#define F_FLUSH  (1u << 9)              /* it has a write cache to flush */

#define T_IN     0                      /* read */
#define T_OUT    1                      /* write */
#define T_FLUSH  4

#define D_NEXT   1                      /* descriptor flags */
#define D_WRITE  2                      /* the device writes this piece */

#define QMAX     256                    /* biggest queue we have room for */
#define TIMEOUT  5000                   /* ms */

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct vring_used_elem {
    uint32_t id, len;
};

/* desc[QMAX], then the available ring; the used ring starts on the next
   page. 256 entries need 10 KiB of physically contiguous memory: the
   kernel's .bss is. */
static uint8_t ring[3 * 4096] __attribute__((aligned(4096)));

static struct {
    uint16_t io;
    uint16_t qsize;
    uint16_t last_used;                 /* used-ring entries we've seen */
    uint8_t busy;
    volatile uint8_t done;
    struct vring_desc *desc;
    volatile uint16_t *avail;           /* flags, idx, ring[qsize] */
    volatile uint16_t *used;            /* flags, idx, then elements */
    struct wait_queue turn, irq_wait;
} vq;

static struct {
    uint32_t type, reserved;
    uint64_t sector;
} hdr;
static volatile uint8_t status;

static void vblk_irq(struct regs *r)
{
    (void)r;
    if (inb(vq.io + VIO_ISR) & 1) {     /* reading it clears it */
        vq.done = 1;
        wq_wake_all(&vq.irq_wait);
    }
}

/* One request: header, the data pieces, status. Sleeps. 0 or -1. */
static int submit(uint32_t type, uint32_t lba, const struct bio_vec *v, int nv)
{
    uint32_t flags = irq_save();
    while (vq.busy)
        wq_sleep(&vq.turn);
    vq.busy = 1;

    hdr.type = type;
    hdr.reserved = 0;
    hdr.sector = lba;
    status = 0xFF;
    struct vring_desc *d = vq.desc;
    d[0] = (struct vring_desc){ V2P(&hdr), sizeof hdr, D_NEXT, 1 };
    for (int k = 0; k < nv; k++)
        d[1 + k] = (struct vring_desc){ v[k].phys, v[k].len,
                                        D_NEXT | (type == T_IN ? D_WRITE : 0), 2 + k };
    d[1 + nv] = (struct vring_desc){ V2P((void *)&status), 1, D_WRITE, 0 };

    uint16_t idx = vq.avail[1];
    vq.avail[2 + idx % vq.qsize] = 0;   /* the chain starts at descriptor 0 */
    __asm__ volatile ("" ::: "memory"); /* ring entry before the index */
    vq.avail[1] = idx + 1;
    __asm__ volatile ("" ::: "memory");
    vq.done = 0;
    outw(vq.io + VIO_QUEUE_NOTIFY, 0);

    uint32_t deadline = timer_ms() + TIMEOUT;
    while (vq.used[1] == vq.last_used)
        if (wq_sleep_until(&vq.irq_wait, deadline) && vq.used[1] == vq.last_used)
            break;
    int err = vq.used[1] == vq.last_used || status != 0 ? -1 : 0;
    if (vq.used[1] != vq.last_used)
        vq.last_used++;
    else
        kprintf("virtio-blk: request timed out\n");

    vq.busy = 0;
    wq_wake_all(&vq.turn);
    irq_restore(flags);
    return err;
}

static int vblk_rwv(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int n, int write)
{
    (void)dev;
    return submit(write ? T_OUT : T_IN, lba, v, n);
}

/* A plain buffer: map it to pieces, at most 128 KiB per request. */
static int rw(uint32_t type, uint32_t lba, uint32_t count, const void *buf)
{
    struct bio_vec v[BIO_VEC_MAX];
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = count > BIO_SECTORS ? BIO_SECTORS : count;
        int nv = blockdev_map(p, n * SECTOR_SIZE, v, BIO_VEC_MAX);
        if (nv < 0 || submit(type, lba, v, nv) < 0)
            return -1;
        lba += n;
        count -= n;
        p += n * SECTOR_SIZE;
    }
    return 0;
}

static int vblk_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    (void)dev;
    return rw(T_IN, lba, count, buf);
}

static int vblk_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    return rw(T_OUT, lba, count, buf);
}

static int vblk_flush(struct blockdev *dev)
{
    (void)dev;
    return submit(T_FLUSH, 0, NULL, 0);
}

void virtio_blk_init(void)
{
    struct pci_dev *pci = pci_find(VIRTIO_VENDOR, VIRTIO_BLK_DEV);
    if (!pci || !(pci->bar[0] & 1))
        return;
    vq.io = pci->bar[0] & ~3u;
    pci_write_config(pci, 4, pci_read_config(pci, 4) | 0x5);   /* I/O + bus master */

    outb(vq.io + VIO_STATUS, 0);                        /* reset */
    outb(vq.io + VIO_STATUS, ST_ACK | ST_DRIVER);
    uint32_t features = inl(vq.io + VIO_DEV_FEATURES);
    outl(vq.io + VIO_GUEST_FEATURES, features & F_FLUSH);

    outw(vq.io + VIO_QUEUE_SEL, 0);
    vq.qsize = inw(vq.io + VIO_QUEUE_SIZE);
    if (vq.qsize < BIO_VEC_MAX + 2 || vq.qsize > QMAX) {
        kprintf("virtio-blk: queue of %u entries not supported\n", vq.qsize);
        outb(vq.io + VIO_STATUS, 0x80);                 /* FAILED */
        return;
    }
    memset(ring, 0, sizeof ring);
    vq.desc = (struct vring_desc *)ring;
    vq.avail = (volatile uint16_t *)(ring + 16 * vq.qsize);
    uint32_t used_off = (16 * vq.qsize + 6 + 2 * vq.qsize + 4095) & ~4095u;
    vq.used = (volatile uint16_t *)(ring + used_off);
    outl(vq.io + VIO_QUEUE_PFN, V2P(ring) / 4096);

    irq_install(pci->irq, vblk_irq);
    outb(vq.io + VIO_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);

    uint32_t lo = inl(vq.io + VIO_CONFIG), hi = inl(vq.io + VIO_CONFIG + 4);
    struct blockdev dev = {
        .name    = "vda",
        .sectors = hi ? 0xFFFFFFFFu : lo,
        .read    = vblk_read,
        .write   = (features & F_RO) ? NULL : vblk_write,
        .flush   = (features & F_FLUSH) ? vblk_flush : NULL,
        .rwv     = vblk_rwv,
    };
    blockdev_register(&dev);
    klog("virtio-blk: vda: %lu MiB, queue %u, irq %u\n",
            dev.sectors / 2048, vq.qsize, pci->irq);
}
