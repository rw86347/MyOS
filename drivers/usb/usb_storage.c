/*
 * usb_storage.c — USB sticks and USB disks: the mass storage class,
 * "bulk-only transport" (BOT). docs/USB-PLAN.md.
 *
 * A USB stick is a SCSI disk at the other end of two pipes (a bulk-out
 * and a bulk-in endpoint). Every disk command is three steps:
 *   1. a 31-byte Command Block Wrapper out: "USBC", a tag, how many bytes
 *      will move and which way, and the SCSI command itself (READ(10)...);
 *   2. the data, in or out;
 *   3. a 13-byte Command Status Wrapper in: "USBS", the same tag, how many
 *      bytes did not move, and the status (0 good, 1 failed, 2 phase error).
 * The stick keeps the steps in order itself, so the command and the data
 * are queued at once and only the end of the data interrupts; then the
 * status is asked for. Two interrupts per disk command (Linux's
 * usb-storage takes three). Asking for the status early as well would
 * save one more, but that is not what Linux or Windows do, so devices
 * (and QEMU's emulated stick, which then never answers) are not tested
 * for it. The data moves straight between the caller's pages and the
 * stick (the xHCI takes a list of pieces), with no copy.
 *
 * SCSI commands used: INQUIRY (who are you), TEST UNIT READY, REQUEST
 * SENSE (why not), READ CAPACITY(10) (how big), READ(10), WRITE(10) and
 * SYNCHRONIZE CACHE(10) (flush).
 *
 * Errors, as the BOT spec says (6.7): a stalled endpoint is cleared and
 * the status read; anything stranger gets a "reset recovery" (the class's
 * reset request, then both endpoints cleared).
 *
 * Not done: LUNs other than 0 (card readers with several slots), sectors
 * other than 512 bytes, UAS (USB 3's faster protocol), write protection.
 */
#include <blockdev.h>
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <usb.h>

#define CBW_SIG  0x43425355u            /* "USBC" */
#define CSW_SIG  0x53425355u            /* "USBS" */
#define TIMEOUT  10000                  /* ms: a slow stick, writing */
#define REPLY    128                    /* in the page: small replies (INQUIRY...) */

struct cbw {
    uint32_t sig, tag, len;
    uint8_t flags, lun, cblen;
    uint8_t cb[16];
} __attribute__((packed));

struct csw {
    uint32_t sig, tag, residue;
    uint8_t status;
} __attribute__((packed));

struct msd {
    struct usb_device *dev;
    uint8_t in, out, iface;
    uint8_t no_sync;                    /* SYNCHRONIZE CACHE refused: don't ask */
    uint16_t max_sectors;               /* per command */
    uint32_t tag;
    uint32_t residue;                   /* the last command's bytes not moved */
    uint8_t *page;                      /* the CBW at 0, the CSW at 64, replies at REPLY */
    uint32_t phys;
    uint8_t busy;
    struct wait_queue turn;
};

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void reset_recovery(struct msd *m)
{
    usb_control(m->dev, 0x21, 0xFF, 0, m->iface, NULL, 0);     /* Bulk-Only Mass Storage Reset */
    usb_clear_halt(m->dev, m->in);
    usb_clear_halt(m->dev, m->out);
}

/* Ask for the status (13 bytes in). 0 when it is in the page. */
static int read_csw(struct msd *m)
{
    struct bio_vec sv = { m->phys + 64, 13 };
    if (usb_queue(m->dev, m->in, &sv, 1, 1) < 0)
        return -1;
    usb_start(m->dev, m->in);
    return usb_wait(m->dev, m->in, TIMEOUT);
}

/* After a failed transfer: clear whatever stalled, and get the status.
   A stalled data stage is the device saying "no more data" (BOT 6.7.2).
   0 if the CSW is now in the page. */
static int recover(struct msd *m, int why)
{
    struct usb_device *d = m->dev;
    if (why > 0) {                      /* an endpoint stopped (a stall, an error) */
        if (usb_halted(d, m->out) && usb_clear_halt(d, m->out) < 0)
            goto reset;
        if (usb_halted(d, m->in) && usb_clear_halt(d, m->in) < 0)
            goto reset;
        if (read_csw(m) == 0)
            return 0;
    }
reset:
    reset_recovery(m);
    return -1;
}

/* One SCSI command, with `len` bytes of data in the pieces v. 0 good, 1
   the device said no (REQUEST SENSE says why), -1 it did not work. */
static int scsi(struct msd *m, const uint8_t *cb, int cblen, const struct bio_vec *v, int nv,
                uint32_t len, int in)
{
    struct usb_device *d = m->dev;
    struct cbw *c = (struct cbw *)m->page;
    struct csw *s = (struct csw *)(m->page + 64);
    memset(c, 0, sizeof *c);
    c->sig = CBW_SIG;
    c->tag = ++m->tag;
    c->len = len;
    c->flags = in ? 0x80 : 0;
    c->cblen = (uint8_t)cblen;
    memcpy(c->cb, cb, cblen);
    memset(s, 0, sizeof *s);
    /* The command, then the data; wait for the last of them. */
    struct bio_vec cv = { m->phys, 31 };
    uint8_t last = !len ? m->out : in ? m->in : m->out;
    if (usb_queue(d, m->out, &cv, 1, !len) < 0 ||
        (len && usb_queue(d, last, v, nv, 1) < 0))
        return -1;
    usb_start(d, m->out);
    if (last != m->out)
        usb_start(d, m->in);
    int r = usb_wait(d, last, TIMEOUT);
    if (r == 0)
        r = read_csw(m);
    if (r && recover(m, r) < 0)
        return -1;
    if (s->sig != CSW_SIG || s->tag != c->tag || s->status > 1) {
        reset_recovery(m);              /* out of step (a phase error) */
        return -1;
    }
    m->residue = s->residue;
    return s->status;
}

/* Why the last command failed: the sense key and the "additional sense
   code" (2/0x3A: no medium; 6: something changed, try again). */
static int sense(struct msd *m, uint8_t *key, uint8_t *asc)
{
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    struct bio_vec v = { m->phys + REPLY, 18 };
    memset(m->page + REPLY, 0, 18);
    if (scsi(m, cb, 6, &v, 1, 18, 1) != 0)
        return -1;
    *key = m->page[REPLY + 2] & 0xF;
    *asc = m->page[REPLY + 12];
    return 0;
}

static void lock(struct msd *m)
{
    uint32_t flags = irq_save();
    while (m->busy)
        wq_sleep(&m->turn);
    m->busy = 1;
    irq_restore(flags);
}

static void unlock(struct msd *m)
{
    uint32_t flags = irq_save();
    m->busy = 0;
    wq_wake_all(&m->turn);
    irq_restore(flags);
}

/* READ(10) / WRITE(10) of the pieces v, max_sectors at a time. */
static int xfer(struct msd *m, uint32_t lba, const struct bio_vec *v, int nv, int write)
{
    struct bio_vec part[BIO_VEC_MAX];
    int k = 0, err = 0;
    uint32_t off = 0;                   /* how far into v[k] */
    lock(m);
    while (k < nv && !err) {
        uint32_t room = m->max_sectors * SECTOR_SIZE, bytes = 0;
        int np = 0;
        while (k < nv && room && np < BIO_VEC_MAX) {
            uint32_t take = v[k].len - off;
            if (take > room)
                take = room;
            part[np++] = (struct bio_vec){ v[k].phys + off, take };
            bytes += take;
            room -= take;
            off += take;
            if (off == v[k].len) {
                k++;
                off = 0;
            }
        }
        uint32_t n = bytes / SECTOR_SIZE;
        uint8_t cb[10] = { write ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        int r = scsi(m, cb, 10, part, np, bytes, !write);
        if (r == 0 && m->residue)
            r = -1;                     /* it came up short */
        if (r) {
            uint8_t key = 0, asc = 0;
            if (r > 0)
                sense(m, &key, &asc);
            kprintf("usb-storage: %s of sector %lu failed (sense %x/%02x)\n",
                    write ? "write" : "read", (unsigned long)lba, key, asc);
            err = -1;
        }
        lba += n;
    }
    unlock(m);
    return err;
}

static int msd_rwv(struct blockdev *bd, uint32_t lba, const struct bio_vec *v, int nv, int write)
{
    return xfer(bd->priv, lba, v, nv, write);
}

static int rw(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf, int write)
{
    struct bio_vec v[BIO_VEC_MAX];
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = count > BIO_SECTORS ? BIO_SECTORS : count;
        int nv = blockdev_map(p, n * SECTOR_SIZE, v, BIO_VEC_MAX);
        if (nv < 0 || xfer(bd->priv, lba, v, nv, write) < 0)
            return -1;
        lba += n;
        count -= n;
        p += n * SECTOR_SIZE;
    }
    return 0;
}

static int msd_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    return rw(bd, lba, count, buf, 0);
}

static int msd_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf)
{
    return rw(bd, lba, count, buf, 1);
}

static int msd_flush(struct blockdev *bd)
{
    struct msd *m = bd->priv;
    if (m->no_sync)
        return 0;
    lock(m);
    uint8_t cb[10] = { 0x35 };          /* SYNCHRONIZE CACHE(10): all of it */
    int r = scsi(m, cb, 10, NULL, 0, 0, 0);
    uint8_t key = 0, asc = 0;
    if (r > 0 && sense(m, &key, &asc) == 0 && key == 5) {
        m->no_sync = 1;                 /* ILLEGAL REQUEST: it has no cache to flush */
        r = 0;
    }
    unlock(m);
    return r ? -1 : 0;
}

/* Copy a SCSI text field without its trailing spaces. */
static void field(char *out, const uint8_t *p, int n)
{
    while (n && p[n - 1] == ' ')
        n--;
    for (int i = 0; i < n; i++)
        out[i] = (p[i] < ' ' || p[i] > '~') ? '?' : (char)p[i];
    out[n] = 0;
}

int usb_storage_probe(struct usb_device *d, const uint8_t *cfg, int len)
{
    const struct usb_interface_descriptor *intf = NULL;
    const struct usb_endpoint_descriptor *last = NULL;
    struct usb_ep_want eps[2] = { { 0 } };      /* in, out */
    for (int o = 0; o + 2 <= len && cfg[o] >= 2 && o + cfg[o] <= len; o += cfg[o]) {
        const uint8_t *x = cfg + o;
        if (x[1] == USB_DT_INTERFACE && x[0] >= 9) {
            if (intf)
                break;                  /* the next interface (or UAS, alt 1) */
            const struct usb_interface_descriptor *i = (const void *)x;
            if (i->bInterfaceClass == USB_CLASS_MASS_STORAGE && i->bInterfaceSubClass == 6 &&
                i->bInterfaceProtocol == 0x50 && i->bAlternateSetting == 0)
                intf = i;               /* SCSI, bulk-only */
        } else if (intf && x[1] == USB_DT_ENDPOINT && x[0] >= 7) {
            const struct usb_endpoint_descriptor *e = (const void *)x;
            int k = (e->bEndpointAddress & USB_DIR_IN) ? 0 : 1;
            last = NULL;
            if ((e->bmAttributes & 3) == USB_ENDPOINT_XFER_BULK && !eps[k].desc)
                eps[k].desc = last = e;
        } else if (last && x[1] == USB_DT_SS_EP_COMP && x[0] >= 6) {
            eps[(last->bEndpointAddress & USB_DIR_IN) ? 0 : 1].burst =
                ((const struct usb_ss_ep_comp_descriptor *)x)->bMaxBurst;
        }
    }
    if (!intf)
        return -1;
    if (!eps[0].desc || !eps[1].desc) {
        kprintf("usb-storage: no bulk endpoints\n");
        return 0;
    }
    if (usb_configure(d, ((const struct usb_config_descriptor *)cfg)->bConfigurationValue,
                      eps, 2) < 0) {
        kprintf("usb-storage: could not configure it\n");
        return 0;
    }
    struct msd *m = kmalloc(sizeof *m);
    uint32_t phys = pmm_alloc();
    if (!m || !phys) {
        kfree(m);
        return 0;
    }
    memset(m, 0, sizeof *m);
    m->dev = d;
    m->in = eps[0].desc->bEndpointAddress;
    m->out = eps[1].desc->bEndpointAddress;
    m->iface = intf->bInterfaceNumber;
    m->phys = phys;
    m->page = P2V(phys);
    /* How much one command moves: 64 KiB on USB 2 (what Windows uses;
       some older sticks fail bigger ones), 128 KiB on USB 3. */
    m->max_sectors = usb_speed(d) >= USB_SPEED_SUPER ? BIO_SECTORS : 128;

    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    struct bio_vec v = { phys + REPLY, 36 };
    char vendor[9], product[17], rev[5];
    if (scsi(m, inq, 6, &v, 1, 36, 1) != 0) {
        kprintf("usb-storage: no answer to INQUIRY\n");
        goto drop;
    }
    uint8_t *r = m->page + REPLY;
    field(vendor, r + 8, 8);
    field(product, r + 16, 16);
    field(rev, r + 32, 4);
    if ((r[0] & 0x1F) != 0) {
        kprintf("usb-storage: %s %s is not a disk (SCSI type %d)\n", vendor, product, r[0] & 0x1F);
        goto drop;
    }

    /* Ready? A stick says "something changed" (a reset) once, and may need
       a moment to start; a card reader without a card says "no medium". */
    for (int t = 0; ; t++) {
        uint8_t tur[6] = { 0 }, key = 0, asc = 0;
        int rc = scsi(m, tur, 6, NULL, 0, 0, 0);
        if (rc == 0)
            break;
        if (rc < 0 || t == 30 || sense(m, &key, &asc) < 0) {
            kprintf("usb-storage: %s %s never became ready\n", vendor, product);
            goto drop;
        }
        if (key == 2 && asc == 0x3A) {
            kprintf("usb-storage: %s %s: no medium\n", vendor, product);
            goto drop;
        }
        if (key == 2)
            usb_sleep(100);             /* NOT READY: still starting */
    }
    uint8_t cap[10] = { 0x25 };
    v.len = 8;
    uint8_t key, asc;
    if (scsi(m, cap, 10, &v, 1, 8, 1) != 0 &&
        (sense(m, &key, &asc), scsi(m, cap, 10, &v, 1, 8, 1) != 0)) {
        kprintf("usb-storage: %s %s: no READ CAPACITY\n", vendor, product);
        goto drop;
    }
    uint32_t lastlba = be32(r), bsize = be32(r + 4);
    if (bsize != SECTOR_SIZE) {
        kprintf("usb-storage: %s %s has %lu-byte sectors: only 512 so far\n", vendor, product,
                (unsigned long)bsize);
        goto drop;
    }
    struct blockdev bd = {
        .sectors   = lastlba == 0xFFFFFFFFu ? lastlba : lastlba + 1,
        .read      = msd_read,
        .write     = msd_write,
        .flush     = msd_flush,
        .rwv       = msd_rwv,
        .priv      = m,
        .removable = 1,                 /* a stick: the installer won't offer it */
    };
    size_t n = strlcpy(bd.model, vendor, sizeof bd.model);   /* "SanDisk Cruzer" */
    if (n && product[0])
        bd.model[n++] = ' ';
    strlcpy(bd.model + n, product, sizeof bd.model - n);
    blockdev_sd_name(bd.name);
    if (!blockdev_register(&bd)) {
        kprintf("usb-storage: no room for another disk\n");
        goto drop;
    }
    kprintf("usb-storage: %s: %s %s %s, %lu MiB\n", bd.name, vendor, product, rev,
            (unsigned long)(bd.sectors / 2048));
    return 0;
drop:                                   /* ours, but of no use */
    pmm_free(phys);
    kfree(m);
    return 0;
}
