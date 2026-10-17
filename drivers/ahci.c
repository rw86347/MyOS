/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * ahci.c — SATA disks through an AHCI controller (ICH6 and later, QEMU's q35)
 *
 * AHCI is how a PC talks SATA natively (instead of pretending to be IDE).
 * The controller ("HBA") has up to 32 ports, one disk each. Its registers
 * are memory-mapped (PCI BAR 5, "ABAR"). Per port, the driver gives it
 * three things in RAM:
 *
 *   command list   32 command headers; we use only slot 0 (one command at
 *                  a time per disk, like the IDE driver)
 *   received FIS   where the disk's answers land
 *   command table  the command itself — a "FIS", the same 7 registers an
 *                  IDE drive has, as a 20-byte packet — plus a scatter-gather
 *                  list (PRDT) of memory pieces, up to 4 MiB each
 *
 * Issuing a command is one MMIO write (PxCI = 1). The HBA moves the data by
 * DMA and interrupts. No 64 KiB boundary rule, no port I/O: on the hot path
 * the CPU writes the 20-byte FIS, the PRD entries and two registers.
 *
 * Not done: the BIOS hand-off (BOHC) some boards need, NCQ (many commands
 * in flight), hot plug, port multipliers, ATAPI.
 */
#include <ahci.h>
#include <blockdev.h>
#include <interrupts.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pci.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <timer.h>

#define PORTS_MAX  4                    /* disks we drive */
#define TIMEOUT    5000                 /* ms */
#define POLL_MS    1                    /* without the interrupt: look this often */
#define CHECK_MS   50                   /* with it: look anyway, now and then */
#define QUIET_MAX  16                   /* commands in a row with no interrupt: polling */

/* HBA registers */
#define CAP   0x00
#define GHC   0x04
#define IS    0x08
#define PI    0x0C
#define GHC_AE  (1u << 31)              /* AHCI mode (not legacy IDE) */
#define GHC_IE  (1u << 1)

/* Port registers, at 0x100 + port * 0x80 */
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1u << 0)              /* process the command list */
#define CMD_FRE  (1u << 4)              /* accept FISes from the disk */
#define CMD_FR   (1u << 14)             /* ...running */
#define CMD_CR   (1u << 15)             /* command list running */
#define IS_ERR   0x78000000u            /* task file / bus / interface errors */
#define IE_BITS  (0x7u | IS_ERR)        /* "done" (D2H, PIO setup, DMA setup FIS), errors */
#define TFD_BUSY 0x88                   /* BSY or DRQ */
#define TFD_ERR  0x01
#define SIG_DISK 0x00000101u

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA
#define ATA_IDENTIFY       0xEC

struct cmd_header {
    uint16_t flags;                     /* FIS length (dwords), W = write */
    uint16_t prdtl;                     /* PRDT entries */
    volatile uint32_t prdbc;            /* bytes moved (the HBA writes it) */
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
};

struct prdt_entry {
    uint32_t dba, dbau, reserved;
    uint32_t dbc;                       /* bytes - 1; bit 31: interrupt */
};

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prdt_entry prdt[BIO_VEC_MAX];
};

/* One page per port: command list (1 KiB-aligned), received FIS, table. */
struct port_mem {
    struct cmd_header list[32];         /* 1024 */
    uint8_t fis[256];                   /* 1024 */
    struct cmd_table table;             /* 1280: 128-byte aligned */
};

struct port {
    volatile uint32_t *r;               /* this port's registers */
    struct port_mem *mem;
    uint32_t mem_phys;
    volatile uint32_t is;               /* status bits the IRQ saw */
    uint8_t num;                        /* port number on the HBA */
    uint8_t busy;
    struct wait_queue turn, irq_wait;
};

static volatile uint32_t *hba;
static struct port ports[PORTS_MAX];
static int nports;
static int polling;                     /* the interrupt doesn't come: poll */
static uint32_t irqs, quiet;            /* interrupts seen; commands in a row without one */

static void ahci_irq(struct regs *r)
{
    (void)r;
    uint32_t is = hba[IS / 4];
    if (!is)
        return;                         /* another device on this line */
    irqs++;
    /* Every port that asks is answered, ours or not yet: a port being set
       up (add_port) interrupts as soon as the disk says hello, and a bit
       left set would raise the interrupt again at once, forever. */
    for (uint32_t bits = is; bits; bits &= bits - 1) {
        int n = __builtin_ctz(bits);
        volatile uint32_t *r = hba + (0x100 + n * 0x80) / 4;
        uint32_t pis = r[PX_IS / 4];
        r[PX_IS / 4] = pis;             /* write 1 to clear */
        for (int k = 0; k < nports; k++)
            if (ports[k].num == n) {
                ports[k].is |= pis;
                wq_wake_all(&ports[k].irq_wait);
            }
    }
    hba[IS / 4] = is;
}

static int wait_clear(volatile uint32_t *reg, uint32_t bits, uint32_t ms)
{
    uint32_t end = timer_ms() + ms;
    while (*reg & bits)
        if ((int32_t)(timer_ms() - end) > 0)
            return -1;
    return 0;
}

static void port_stop(struct port *p)
{
    p->r[PX_CMD / 4] &= ~CMD_ST;
    wait_clear(&p->r[PX_CMD / 4], CMD_CR, 500);
    p->r[PX_CMD / 4] &= ~CMD_FRE;
    wait_clear(&p->r[PX_CMD / 4], CMD_FR, 500);
}

static void port_start(struct port *p)
{
    p->r[PX_SERR / 4] = 0xFFFFFFFFu;
    p->r[PX_IS / 4] = 0xFFFFFFFFu;
    p->r[PX_CMD / 4] |= CMD_FRE;
    wait_clear(&p->r[PX_TFD / 4], TFD_BUSY, 1000);
    p->r[PX_CMD / 4] |= CMD_ST;
}

/* One command in slot 0: the FIS, the data pieces, wait for the interrupt.
   Sleeps. 0 or -1 (after an error the port is restarted). */
static int issue(struct port *p, uint8_t cmd, uint64_t lba, uint32_t count,
                 const struct bio_vec *v, int nv, int write)
{
    uint32_t flags = irq_save();
    while (p->busy)
        wq_sleep(&p->turn);
    p->busy = 1;

    struct cmd_table *t = &p->mem->table;
    uint8_t *f = t->cfis;
    memset(f, 0, 20);
    f[0] = 0x27;                        /* host-to-device register FIS */
    f[1] = 0x80;                        /* it carries a command */
    f[2] = cmd;
    f[4] = lba;
    f[5] = lba >> 8;
    f[6] = lba >> 16;
    f[7] = 0x40;                        /* LBA mode */
    f[8] = lba >> 24;
    f[9] = lba >> 32;
    f[10] = lba >> 40;
    f[12] = count;
    f[13] = count >> 8;
    for (int k = 0; k < nv; k++)
        t->prdt[k] = (struct prdt_entry){ v[k].phys, 0, 0, v[k].len - 1 };
    struct cmd_header *h = &p->mem->list[0];
    h->flags = 5 | (write ? 0x40 : 0);  /* 5 dwords of FIS */
    h->prdtl = nv;
    h->prdbc = 0;

    p->is = 0;
    __asm__ volatile ("" ::: "memory");
    p->r[PX_CI / 4] = 1;
    /* Sleep until the interrupt says done, but look at the port now and
       then too, so that a machine whose interrupt never arrives (it
       happened: QEMU's q35 under UEFI, on the 8259 line) isn't stuck.
       After QUIET_MAX commands in a row that ended with no interrupt at
       all, it says so once and looks every POLL_MS: slow, not 5 s a
       command. (One command ending on a look proves nothing: the look's
       timer tick and the interrupt can come together, the tick first.) */
    uint32_t deadline = timer_ms() + TIMEOUT, seen = irqs;
    while ((p->r[PX_CI / 4] & 1) && !(p->is & IS_ERR)) {
        uint32_t now = timer_ms(), next = now + (polling ? POLL_MS : CHECK_MS);
        if ((int32_t)(now - deadline) >= 0)
            break;
        if ((int32_t)(next - deadline) > 0)
            next = deadline;
        if (wq_sleep_until(&p->irq_wait, next)) {
            uint32_t pis = p->r[PX_IS / 4];
            p->r[PX_IS / 4] = pis;      /* what the interrupt handler would have seen */
            p->is |= pis;
        }
    }
    if (irqs != seen) {
        quiet = 0;
    } else if (++quiet == QUIET_MAX && !polling) {
        kprintf("ahci: its interrupt does not arrive: polling every %d ms\n", POLL_MS);
        polling = 1;
    }
    int err = (p->r[PX_CI / 4] & 1) || (p->is & IS_ERR) || (p->r[PX_TFD / 4] & TFD_ERR);
    if (err) {
        kprintf("ahci: command %x failed (is %lx, tfd %lx)\n", cmd, (unsigned long)p->is,
                (unsigned long)p->r[PX_TFD / 4]);
        port_stop(p);
        port_start(p);
    }
    p->busy = 0;
    wq_wake_all(&p->turn);
    irq_restore(flags);
    return err ? -1 : 0;
}

static int ahci_rwv(struct blockdev *dev, uint32_t lba, const struct bio_vec *v, int nv, int write)
{
    uint32_t count = 0;
    for (int k = 0; k < nv; k++)
        count += v[k].len / SECTOR_SIZE;
    return issue(dev->priv, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT,
                 lba, count, v, nv, write);
}

static int rw(struct port *p, uint32_t lba, uint32_t count, const void *buf, int write)
{
    struct bio_vec v[BIO_VEC_MAX];
    const uint8_t *b = buf;
    while (count) {
        uint32_t n = count > BIO_SECTORS ? BIO_SECTORS : count;
        int nv = blockdev_map(b, n * SECTOR_SIZE, v, BIO_VEC_MAX);
        if (nv < 0 || issue(p, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT,
                            lba, n, v, nv, write) < 0)
            return -1;
        lba += n;
        count -= n;
        b += n * SECTOR_SIZE;
    }
    return 0;
}

static int ahci_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    return rw(dev->priv, lba, count, buf, 0);
}

static int ahci_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    return rw(dev->priv, lba, count, buf, 1);
}

static int ahci_flush(struct blockdev *dev)
{
    return issue(dev->priv, ATA_FLUSH_EXT, 0, 0, NULL, 0, 0);
}

static void add_port(int n)
{
    struct port *p = &ports[nports];
    p->r = hba + (0x100 + n * 0x80) / 4;
    p->num = n;
    if ((p->r[PX_SSTS / 4] & 0xF) != 3)
        return;                         /* nothing there */
    uint32_t frame = pmm_alloc(), idframe = pmm_alloc();
    if (!frame || !idframe) {
        if (frame)
            pmm_free(frame);
        if (idframe)
            pmm_free(idframe);
        return;
    }
    p->mem = P2V(frame);
    p->mem_phys = frame;
    memset(p->mem, 0, PAGE_SIZE);

    port_stop(p);
    p->r[PX_CLB / 4] = frame;
    p->r[PX_CLBU / 4] = 0;
    p->r[PX_FB / 4] = frame + 1024;
    p->r[PX_FBU / 4] = 0;
    p->mem->list[0].ctba = frame + 1280;
    p->r[PX_IE / 4] = IE_BITS;
    port_start(p);
    /* What the device is (its signature) comes in its first register FIS,
       which the port takes only once FIS receiving is on (port_start). A
       port the firmware never used, or reset, still says 0xFFFFFFFF until
       then (QEMU's always does), so it is read only now, after waiting a
       moment for a slow device. */
    uint32_t end = timer_ms() + 100;
    while (p->r[PX_SIG / 4] == 0xFFFFFFFFu && (int32_t)(timer_ms() - end) < 0)
        ;
    if (p->r[PX_SIG / 4] != SIG_DISK) { /* a CD drive, or no answer */
        port_stop(p);
        pmm_free(frame);
        pmm_free(idframe);
        return;
    }
    nports++;

    uint16_t *id = P2V(idframe);
    struct bio_vec v = { idframe, 512 };
    if (issue(p, ATA_IDENTIFY, 0, 0, &v, 1, 0) < 0 || !(id[83] & (1u << 10))) {
        kprintf("ahci: port %d: IDENTIFY failed or no LBA48\n", n);
        nports--;
        port_stop(p);
        pmm_free(frame);
        pmm_free(idframe);
        return;
    }
    uint32_t sectors = id[102] || id[103] ? 0xFFFFFFFFu : ((uint32_t)id[101] << 16 | id[100]);
    struct blockdev dev = {
        .sectors = sectors,
        .read    = ahci_read,
        .write   = ahci_write,
        .flush   = ahci_flush,
        .rwv     = ahci_rwv,
        .priv    = p,
    };
    /* The model: IDENTIFY words 27-46, two characters per word with their
       bytes swapped, padded with spaces (as ata.c reads it). */
    for (int k = 0; k < 20; k++) {
        dev.model[2 * k] = (char)(id[27 + k] >> 8);
        dev.model[2 * k + 1] = (char)(id[27 + k] & 0xFF);
    }
    for (int k = 40; k > 0 && (dev.model[k - 1] == ' ' || !dev.model[k - 1]); k--)
        dev.model[k - 1] = 0;
    pmm_free(idframe);
    blockdev_sd_name(dev.name);
    blockdev_register(&dev);
    klog("ahci: %s: port %d, %lu MiB\n", dev.name, n, (unsigned long)(sectors / 2048));
}

void ahci_init(void)
{
    struct pci_dev *pci = NULL;
    for (int i = 0; i < pci_count() && !pci; i++)
        if (pci_dev_at(i)->class_rev >> 8 == 0x010601)
            pci = pci_dev_at(i);
    if (!pci || (pci->bar[5] & 1) || !(pci->bar[5] & ~0xFu))
        return;
    hba = pci_map_mmio(pci->bar[5] & ~0xFu, 0x1100);
    if (!hba)
        return;
    pci_write_config(pci, 4, pci_read_config(pci, 4) | 0x6);  /* memory + bus master */
    hba[GHC / 4] |= GHC_AE;
    /* MSI where the controller has it (its own vector, no shared line to
       route), else the 8259 line. */
    if (pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX) < 0)
        kprintf("ahci: no interrupt: polling\n");
    else
        irq_install(pci_irq_vector(pci, 0), ahci_irq);
    hba[GHC / 4] |= GHC_IE;
    uint32_t pi = hba[PI / 4];
    for (int n = 0; n < 32 && nports < PORTS_MAX; n++)
        if (pi & (1u << n))
            add_port(n);
    if (nports)
        klog("ahci: %04x:%04x, %s irq %d\n", pci->vendor, pci->device,
             pci->irq_type == PCI_IRQ_MSI ? "MSI" : "pin", pci_irq_vector(pci, 0));
}
