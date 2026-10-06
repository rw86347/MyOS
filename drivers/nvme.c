/*
 * nvme.c — NVMe SSDs (PCIe flash; QEMU's -device nvme)
 *
 * NVMe was designed for flash, with no legacy at all: the driver and the
 * SSD share queues in RAM. A command is a 64-byte entry in a submission
 * queue (SQ); the driver writes the new tail index to a "doorbell"
 * register (one MMIO write) and the SSD fetches the entry itself, moves the
 * data by DMA and writes a 16-byte entry to a completion queue (CQ). A bit
 * in each completion (the "phase") flips every pass around the ring, so a
 * new entry is told from an old one without a register read.
 *
 * Data is described by "PRP" entries: physical page addresses. The first
 * may start inside a page; every later one is a whole page, except that
 * the last may end early. A transfer of one or two pages fits in the
 * command; a longer one points at a page-sized list of entries.
 *
 * One I/O queue pair, one command at a time per controller, like the other
 * disk drivers. The interrupt is MSI-X where the PCI code can give one
 * (a message per completion: nothing to mask), else MSI or the pin (INTx).
 * A pin stays raised while a completion is unread, so then the handler
 * masks it (INTMS) and the thread unmasks it after reading the completion;
 * under MSI-X the spec forbids touching INTMS/INTMC at all.
 *
 * Not done: more queues and commands in flight, namespaces other than 1,
 * sector sizes other than 512 bytes.
 */
#include <blockdev.h>
#include <interrupts.h>
#include <kprintf.h>
#include <memlayout.h>
#include <nvme.h>
#include <pci.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <timer.h>

#define QSIZE    16                     /* entries per queue */
#define TIMEOUT  5000                   /* ms, an I/O command */

/* Controller registers */
#define R_CAP    0x00                   /* 64-bit */
#define R_INTMS  0x0C
#define R_INTMC  0x10
#define R_CC     0x14
#define R_CSTS   0x1C
#define R_AQA    0x24
#define R_ASQ    0x28                   /* 64-bit */
#define R_ACQ    0x30                   /* 64-bit */
#define DOORBELLS 0x1000

#define CC_EN      1u
#define CC_QES     ((6u << 16) | (4u << 20))    /* 64-byte SQ, 16-byte CQ entries */
#define CSTS_RDY   1u
#define CSTS_CFS   2u

/* Opcodes */
#define ADM_CREATE_SQ  0x01
#define ADM_CREATE_CQ  0x05
#define ADM_IDENTIFY   0x06
#define IO_FLUSH       0x00
#define IO_WRITE       0x01
#define IO_READ        0x02

struct sqe {
    uint8_t opcode, flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cqe {
    uint32_t result, reserved;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    uint16_t status;                    /* bit 0: phase; then the status code */
};

struct queue {
    struct sqe *sq;
    volatile struct cqe *cq;
    uint32_t sq_phys, cq_phys;
    uint16_t tail, head;
    uint8_t phase;
    volatile uint32_t *sq_bell, *cq_bell;
};

static volatile uint8_t *regs;
static struct queue admin, io;
static uint64_t *prp_list;              /* one page: 512 entries */
static uint32_t prp_list_phys;
static uint8_t busy;
static uint8_t msix;                    /* MSI-X: never touch INTMS/INTMC */
static volatile uint8_t irq_seen;
static struct wait_queue turn, irq_wait;

static uint32_t rd32(uint32_t r) { return *(volatile uint32_t *)(regs + r); }
static void wr32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }
static void wr64(uint32_t r, uint64_t v) { wr32(r, (uint32_t)v); wr32(r + 4, (uint32_t)(v >> 32)); }

static int cq_ready(struct queue *q)
{
    return (q->cq[q->head].status & 1) == q->phase;
}

static void nvme_irq(struct regs *r)
{
    (void)r;
    if (!io.cq || !cq_ready(&io))
        return;                         /* not ours: the line is shared */
    if (!msix)
        wr32(R_INTMS, 1);               /* quiet until the thread has read it */
    irq_seen = 1;
    wq_wake_all(&irq_wait);
}

/* Put a command on q and ring its doorbell. */
static void submit(struct queue *q, const struct sqe *e)
{
    q->sq[q->tail] = *e;
    q->tail = (q->tail + 1) % QSIZE;
    __asm__ volatile ("" ::: "memory");
    *q->sq_bell = q->tail;
}

/* Take the completion at the head; its status code (0: success). */
static uint16_t reap(struct queue *q)
{
    uint16_t st = q->cq[q->head].status >> 1;
    if (++q->head == QSIZE) {
        q->head = 0;
        q->phase ^= 1;
    }
    *q->cq_bell = q->head;
    return st;
}

/* Admin commands run at boot only: polled. */
static int admin_cmd(struct sqe *e)
{
    submit(&admin, e);
    uint32_t end = timer_ms() + 2000;
    while (!cq_ready(&admin))
        if ((int32_t)(timer_ms() - end) > 0)
            return -1;
    return reap(&admin) ? -1 : 0;
}

/* Fill e's PRP fields for pieces v[0..n). Returns how many pieces fit the
   PRP rules in one command (at least 1). */
static int set_prps(struct sqe *e, const struct bio_vec *v, int n)
{
    uint32_t pages[BIO_SECTORS * SECTOR_SIZE / PAGE_SIZE + 2];
    pages[0] = v[0].phys & ~(PAGE_SIZE - 1);
    int np = 0, used = 0;
    for (int k = 0; k < n; k++) {
        uint32_t pa = v[k].phys, end = pa + v[k].len;
        if (k > 0 && (pa & (PAGE_SIZE - 1)))
            break;                      /* a later piece must start a page */
        uint32_t first = pa & ~(PAGE_SIZE - 1);
        if (np + (end - first + PAGE_SIZE - 1) / PAGE_SIZE > (int)(sizeof pages / sizeof *pages))
            break;
        for (uint32_t p = first; p < end; p += PAGE_SIZE)
            pages[np++] = p;
        used = k + 1;
        if (end & (PAGE_SIZE - 1))
            break;                      /* ...and only the last may end early */
    }
    e->prp1 = pages[0] | (v[0].phys & (PAGE_SIZE - 1));
    if (np == 2) {
        e->prp2 = pages[1];
    } else if (np > 2) {
        for (int k = 1; k < np; k++)
            prp_list[k - 1] = pages[k];
        e->prp2 = prp_list_phys;
    }
    return used;
}

/* One I/O command on namespace 1 (PRPs already set). Sleeps. 0 or -1. */
static int io_cmd(struct sqe *e, uint8_t op, uint32_t lba, uint32_t count)
{
    e->opcode = op;
    e->nsid = 1;
    e->cdw10 = lba;
    e->cdw12 = count ? count - 1 : 0;   /* "0's based": 0 means one sector */

    irq_seen = 0;
    submit(&io, e);
    uint32_t deadline = timer_ms() + TIMEOUT;
    while (!cq_ready(&io))
        if (wq_sleep_until(&irq_wait, deadline) && !cq_ready(&io))
            break;
    int err = -1;
    if (cq_ready(&io))
        err = reap(&io) ? -1 : 0;
    else
        kprintf("nvme: command %x timed out\n", op);
    if (!msix)
        wr32(R_INTMC, 1);
    return err;
}

/* Do the pieces fit one command's PRPs? (Every piece but the first starts
   a page, every piece but the last ends one.) */
static int prp_ok(const struct bio_vec *v, int nv)
{
    for (int k = 0; k < nv; k++)
        if ((k > 0 && (v[k].phys & (PAGE_SIZE - 1))) ||
            (k < nv - 1 && ((v[k].phys + v[k].len) & (PAGE_SIZE - 1))))
            return 0;
    return 1;
}

#define BOUNCE_PAGES (BIO_SECTORS * SECTOR_SIZE / PAGE_SIZE)
static uint32_t bounce[BOUNCE_PAGES];   /* frames, taken at boot (128 KiB) */

/* Copy the pieces into the bounce pages, back to back (or out of them). */
static void bounce_copy(const struct bio_vec *v, int nv, int into)
{
    uint32_t at = 0;
    for (int k = 0; k < nv; k++) {
        for (uint32_t done = 0; done < v[k].len; ) {
            uint32_t in = (at + done) % PAGE_SIZE, c = PAGE_SIZE - in;
            if (c > v[k].len - done)
                c = v[k].len - done;
            uint8_t *b = (uint8_t *)P2V(bounce[(at + done) / PAGE_SIZE]) + in;
            uint8_t *p = (uint8_t *)P2V(v[k].phys) + done;
            if (into)
                memcpy(b, p, c);
            else
                memcpy(p, b, c);
            done += c;
        }
        at += v[k].len;
    }
}

/* Pieces that don't fit the PRP rules (the block cache's 1 KiB blocks,
   each in a page of its own) go through whole bounce pages: one copy and
   ONE command instead of a command per piece. Busy is held. */
static int xfer_bounce(uint8_t op, uint32_t lba, const struct bio_vec *v, int nv)
{
    uint32_t bytes = 0;
    for (int k = 0; k < nv; k++)
        bytes += v[k].len;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!bounce[pages - 1])
        return 1;                       /* no bounce pages: piece by piece */
    struct bio_vec bv[BOUNCE_PAGES];
    for (uint32_t k = 0; k < pages; k++)
        bv[k] = (struct bio_vec){ bounce[k], k == pages - 1 ? bytes - k * PAGE_SIZE : PAGE_SIZE };
    if (op == IO_WRITE)
        bounce_copy(v, nv, 1);
    struct sqe e;
    memset(&e, 0, sizeof e);
    set_prps(&e, bv, pages);
    int err = io_cmd(&e, op, lba, bytes / SECTOR_SIZE);
    if (!err && op == IO_READ)
        bounce_copy(v, nv, 0);
    return err;
}

/* Split the pieces into commands that fit the PRP rules. */
static int xfer(uint8_t op, uint32_t lba, const struct bio_vec *v, int nv)
{
    uint32_t flags = irq_save();
    while (busy)
        wq_sleep(&turn);
    busy = 1;
    int err = 0;
    if (nv > 1 && !prp_ok(v, nv)) {
        err = xfer_bounce(op, lba, v, nv);
        if (err <= 0)
            nv = 0;                     /* done (or failed) */
        else
            err = 0;
    }
    while (nv && !err) {
        struct sqe e;
        memset(&e, 0, sizeof e);
        int n = set_prps(&e, v, nv);
        uint32_t count = 0;
        for (int k = 0; k < n; k++)
            count += v[k].len / SECTOR_SIZE;
        err = io_cmd(&e, op, lba, count);
        lba += count;
        v += n;
        nv -= n;
    }
    busy = 0;
    wq_wake_all(&turn);
    irq_restore(flags);
    return err;
}

static int nvme_rwv(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int nv, int write)
{
    (void)dev;
    return xfer(write ? IO_WRITE : IO_READ, lba, v, nv);
}

static int rw(uint8_t op, uint32_t lba, uint32_t count, const void *buf)
{
    struct bio_vec v[BIO_VEC_MAX];
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = count > BIO_SECTORS ? BIO_SECTORS : count;
        int nv = blockdev_map(p, n * SECTOR_SIZE, v, BIO_VEC_MAX);
        if (nv < 0 || xfer(op, lba, v, nv) < 0)
            return -1;
        lba += n;
        count -= n;
        p += n * SECTOR_SIZE;
    }
    return 0;
}

static int nvme_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    (void)dev;
    return rw(IO_READ, lba, count, buf);
}

static int nvme_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    return rw(IO_WRITE, lba, count, buf);
}

static int nvme_flush(struct blockdev *dev)
{
    (void)dev;
    uint32_t flags = irq_save();
    while (busy)
        wq_sleep(&turn);
    busy = 1;
    struct sqe e;
    memset(&e, 0, sizeof e);
    int err = io_cmd(&e, IO_FLUSH, 0, 0);
    busy = 0;
    wq_wake_all(&turn);
    irq_restore(flags);
    return err;
}

static int queue_init(struct queue *q, int qid, uint32_t stride)
{
    q->sq_phys = pmm_alloc();
    q->cq_phys = pmm_alloc();
    if (!q->sq_phys || !q->cq_phys)
        return -1;
    q->sq = P2V(q->sq_phys);
    q->cq = P2V(q->cq_phys);
    memset(q->sq, 0, PAGE_SIZE);
    memset((void *)q->cq, 0, PAGE_SIZE);
    q->tail = q->head = 0;
    q->phase = 1;
    q->sq_bell = (volatile uint32_t *)(regs + DOORBELLS + (2 * qid) * stride);
    q->cq_bell = (volatile uint32_t *)(regs + DOORBELLS + (2 * qid + 1) * stride);
    return 0;
}

static int wait_ready(int want, uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while ((rd32(R_CSTS) & CSTS_RDY) != (uint32_t)want)
        if ((rd32(R_CSTS) & CSTS_CFS) || (int32_t)(timer_ms() - end) > 0)
            return -1;
    return 0;
}

void nvme_init(void)
{
    struct pci_dev *pci = NULL;
    for (int i = 0; i < pci_count() && !pci; i++)
        if (pci_dev_at(i)->class_rev >> 8 == 0x010802)    /* NVM Express */
            pci = pci_dev_at(i);
    phys_addr_t bar = pci ? pci_bar_phys(pci, 0) : 0;
    if (!bar)
        return;                         /* no device, or no memory BAR */
    if (pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_INTX) < 0) {
        kprintf("nvme: no interrupt (no MSI and no 8259 line)\n");
        return;
    }
    msix = pci->irq_type == PCI_IRQ_MSIX;
    uint32_t stride = 0;
    regs = pci_map_mmio(bar, 0x1000);
    if (!regs) {
        kprintf("nvme: cannot map its registers at %08lx%08lx%s\n",
                (unsigned long)(bar >> 32), (unsigned long)bar,
                bar >> 32 ? " (above 4 GiB: a 32-bit kernel can't)" : "");
        return;
    }
    uint32_t cap_hi = rd32(R_CAP + 4);
    stride = 4u << (cap_hi & 0xF);
    uint32_t to_ms = ((rd32(R_CAP) >> 24) & 0xFF) * 500 + 500;
    regs = pci_map_mmio(bar, DOORBELLS + 4 * stride);
    if (!regs)
        return;
    pci_write_config(pci, 4, pci_read_config(pci, 4) | 0x6);  /* memory + bus master */

    wr32(R_CC, 0);
    if (wait_ready(0, to_ms) < 0)
        goto fail;
    if (!msix)
        wr32(R_INTMS, 1);               /* admin completions are polled */
    if (queue_init(&admin, 0, stride) < 0 || queue_init(&io, 1, stride) < 0)
        goto fail;
    wr32(R_AQA, ((QSIZE - 1) << 16) | (QSIZE - 1));
    wr64(R_ASQ, admin.sq_phys);
    wr64(R_ACQ, admin.cq_phys);
    wr32(R_CC, CC_EN | CC_QES);
    if (wait_ready(1, to_ms) < 0)
        goto fail;

    prp_list_phys = pmm_alloc();
    uint32_t idf = pmm_alloc();
    if (!prp_list_phys || !idf)
        goto fail;
    prp_list = P2V(prp_list_phys);

    struct sqe e;
    memset(&e, 0, sizeof e);            /* identify namespace 1 */
    e.opcode = ADM_IDENTIFY;
    e.nsid = 1;
    e.prp1 = idf;
    e.cdw10 = 0;
    if (admin_cmd(&e) < 0)
        goto fail;
    uint8_t *id = P2V(idf);
    uint32_t nsze_lo = *(uint32_t *)id, nsze_hi = *(uint32_t *)(id + 4);
    uint32_t lbaf = *(uint32_t *)(id + 128 + 4 * (id[26] & 0xF));
    pmm_free(idf);
    if (((lbaf >> 16) & 0xFF) != 9) {
        kprintf("nvme: sectors are not 512 bytes\n");
        goto fail;
    }

    memset(&e, 0, sizeof e);            /* I/O completion queue 1, interrupts on */
    e.opcode = ADM_CREATE_CQ;
    e.prp1 = io.cq_phys;
    e.cdw10 = ((QSIZE - 1) << 16) | 1;
    e.cdw11 = 0x3;                      /* physically contiguous, interrupts enabled */
    if (admin_cmd(&e) < 0)
        goto fail;
    memset(&e, 0, sizeof e);            /* I/O submission queue 1, completing to CQ 1 */
    e.opcode = ADM_CREATE_SQ;
    e.prp1 = io.sq_phys;
    e.cdw10 = ((QSIZE - 1) << 16) | 1;
    e.cdw11 = (1u << 16) | 1;
    if (admin_cmd(&e) < 0)
        goto fail;

    for (int k = 0; k < BOUNCE_PAGES && (bounce[k] = pmm_alloc()) != 0; k++)
        ;                               /* short of memory: fewer, used only if all there */
    irq_install(pci_irq_vector(pci, 0), nvme_irq);
    if (!msix)
        wr32(R_INTMC, 1);

    struct blockdev dev = {
        .name    = "nvme0n1",
        .sectors = nsze_hi ? 0xFFFFFFFFu : nsze_lo,
        .read    = nvme_read,
        .write   = nvme_write,
        .flush   = nvme_flush,
        .rwv     = nvme_rwv,
    };
    blockdev_register(&dev);
    kprintf("nvme: nvme0n1: %lu MiB, %s irq %d\n", (unsigned long)(dev.sectors / 2048),
            msix ? "MSI-X" : pci->irq_type == PCI_IRQ_MSI ? "MSI" : "pin",
            pci_irq_vector(pci, 0));
    return;
fail:
    kprintf("nvme: controller did not start (status %lx)\n", rd32(R_CSTS));
    wr32(R_CC, 0);
}
