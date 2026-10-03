/*
 * ata.c — ATA (IDE) hard disk driver, PIO mode, interrupt driven
 *
 * Every PC from ~1990 to ~2008 has IDE (also called PATA) disk controllers,
 * and later SATA chipsets can emulate them ("IDE/legacy mode" in the BIOS).
 * QEMU's and UTM's default PC has one too, so this is our first disk driver.
 *
 * A PC has two IDE "channels", each with up to two drives (master/slave):
 *
 *   channel     command ports   control port   IRQ   Linux name
 *   primary     0x1F0-0x1F7     0x3F6          14    hda (master), hdb (slave)
 *   secondary   0x170-0x177     0x376          15    hdc (master), hdd (slave)
 *
 * "PIO" = Programmed I/O: the CPU copies every 16-bit word between the disk
 * and memory itself (rep insw / rep outsw). Simple, slow (~a few MB/s) and
 * always works. DMA (the disk writes to RAM by itself) comes later.
 *
 * Interrupt driven: after giving the drive a command, the thread that asked
 * SLEEPS. The drive raises its IRQ when a sector is ready (or the command
 * is done) and the interrupt handler wakes the thread. A real hard disk
 * takes milliseconds to seek; meanwhile the CPU runs other programs instead
 * of spinning on a status register. The handler itself only reads the
 * status (which tells the drive "seen") — the copying happens in the thread
 * (CLAUDE.md rule 2: interrupt handlers do the minimum).
 *
 * If a channel's interrupt never arrives (some BIOS setups route it
 * elsewhere), that channel falls back to polling and says so at boot.
 *
 * Addressing: LBA28 — sector numbers up to 2^28 (128 GiB). Enough for now;
 * LBA48 (bigger disks) is a small addition later.
 */
#include <ata.h>
#include <blockdev.h>
#include <interrupts.h>
#include <io.h>
#include <kprintf.h>
#include <thread.h>
#include <timer.h>

/* Registers, as offsets from the channel's command base port. */
#define REG_DATA      0   /* 16-bit data in/out */
#define REG_ERROR     1   /* read: error details */
#define REG_SECCOUNT  2   /* number of sectors to transfer */
#define REG_LBA_LO    3   /* LBA bits 0-7 */
#define REG_LBA_MID   4   /* LBA bits 8-15 */
#define REG_LBA_HI    5   /* LBA bits 16-23 */
#define REG_DRIVE     6   /* drive select + LBA bits 24-27 */
#define REG_STATUS    7   /* read: status (and "interrupt seen") */
#define REG_COMMAND   7   /* write: command */

/* Status register bits */
#define ST_ERR  0x01      /* the last command failed */
#define ST_DRQ  0x08      /* drive is ready to transfer a sector of data */
#define ST_DF   0x20      /* drive fault */
#define ST_BSY  0x80      /* drive is busy: ignore all other bits */

/* Commands */
#define CMD_READ_SECTORS   0x20
#define CMD_WRITE_SECTORS  0x30
#define CMD_READ_MULTIPLE  0xC4     /* one interrupt per block of sectors */
#define CMD_WRITE_MULTIPLE 0xC5
#define CMD_SET_MULTIPLE   0xC6     /* the block size for those two */
#define CMD_CACHE_FLUSH    0xE7
#define CMD_IDENTIFY       0xEC

/* Sectors per interrupt we ask for (drives offer up to 16, some more).
   One interrupt and two thread switches per 8 KiB instead of per 512 B. */
#define MULTI_WANT 16

/* Device control register (the control port) */
#define CTL_NIEN  0x02    /* "no interrupts, please" */

#define SPIN_LIMIT    1000000 /* status reads before giving up (~1 s) */
#ifndef POLL_FIRST
#define POLL_FIRST    2048     /* status reads before sleeping (see wait_drive) */
#endif
#define IRQ_TIMEOUT   5000    /* ms: a disk spinning up from standby is slow */
#define FLUSH_TIMEOUT 30000   /* ms: writing out a drive's whole cache */
#define PROBE_TIMEOUT 200     /* ms: does this channel's IRQ work at all? */

struct ata_channel {
    uint16_t base;                  /* command block base port, e.g. 0x1F0 */
    uint16_t ctrl;                  /* control port, e.g. 0x3F6 */
    uint8_t  irq;                   /* 14 or 15 */
    uint8_t  use_irq;               /* 0: poll (the IRQ didn't work) */
    uint8_t  busy;                  /* a command is running */
    volatile uint8_t irq_seen;      /* the drive interrupted... */
    volatile uint8_t irq_status;    /* ...and this was its status */
    struct wait_queue irq_wait;     /* the thread waiting for the drive */
    struct wait_queue turn;         /* threads waiting for the channel */
};

struct ata_drive {
    struct ata_channel *ch;
    uint8_t slave;                  /* 0 = master, 1 = slave */
    uint8_t multi;                  /* sectors per interrupt (1: plain commands) */
};

static struct ata_channel channels[2];
static struct ata_drive drives[4];

/*
 * After selecting a drive, the spec says wait 400 ns before trusting the
 * status register. Each port read takes ~100 ns, so read the "alternate
 * status" register (same value, but reading it has no side effects) 4 times.
 */
static void delay400ns(struct ata_channel *c)
{
    for (int i = 0; i < 4; i++)
        inb(c->ctrl);
}

/* Spin until BSY clears. Returns the final status, or -1 on timeout. Only
   for moments that are short anyway (the drive accepting a command). */
static int wait_not_busy(struct ata_channel *c)
{
    for (int i = 0; i < SPIN_LIMIT; i++) {
        uint8_t st = inb(c->base + REG_STATUS);
        if (!(st & ST_BSY))
            return st;
    }
    return -1;
}

/* --- interrupts ---------------------------------------------------------- */

/* The drive has a sector ready, or is done. Reading the status register
   tells it we've noticed (it lowers its interrupt line); the waiting
   thread does the rest. */
static void channel_irq(struct ata_channel *c)
{
    c->irq_status = inb(c->base + REG_STATUS);
    c->irq_seen = 1;
    wq_wake_all(&c->irq_wait);
}

static void irq14(struct regs *r) { (void)r; channel_irq(&channels[0]); }
static void irq15(struct regs *r) { (void)r; channel_irq(&channels[1]); }

/*
 * Wait until the drive is no longer busy, and return its status (-1 if it
 * never gets there). First a SHORT spin on the alternate status register
 * (reading it doesn't count as "interrupt seen"): data in the drive's own
 * cache — and every read in QEMU or UTM — is ready within microseconds,
 * and a sleep/wake costs far more there (QEMU takes ~2.5 ms to wake a
 * halted virtual CPU). Only then sleep until the drive's interrupt.
 *
 * The interrupt is just a nudge: after waking we look at the drive again.
 * So an interrupt left over from a block we already took by polling (it
 * arrives later) is harmless — the drive is still busy, and we wait on.
 * (Interrupts are off here: one can't slip in between check and sleep.)
 */
static int wait_drive(struct ata_channel *c, uint32_t timeout_ms)
{
    delay400ns(c);
    if (!c->use_irq)
        return wait_not_busy(c);
    uint32_t deadline = timer_ms() + timeout_ms;
    for (;;) {
        for (int i = 0; i < POLL_FIRST; i++)
            if (!(inb(c->ctrl) & ST_BSY))
                return inb(c->base + REG_STATUS);   /* (and clear the IRQ) */
        c->irq_seen = 0;
        if (wq_sleep_until(&c->irq_wait, deadline) && !c->irq_seen)
            return -1;                  /* timed out */
    }
}

static int bad(int st) { return st < 0 || (st & (ST_ERR | ST_DF)); }

/* One command at a time per channel (master and slave share the cable).
   A sleeping lock: interrupts are off, so test-and-set can't be split. */
static void channel_take(struct ata_channel *c)
{
    while (c->busy)
        wq_sleep(&c->turn);
    c->busy = 1;
}

static void channel_give(struct ata_channel *c)
{
    c->busy = 0;
    wq_wake_all(&c->turn);
}

/* --- commands --------------------------------------------------------------- */

/* Select the drive, load the LBA28 address + sector count, send `cmd`. */
static int start(struct ata_drive *d, uint32_t lba, uint32_t count, uint8_t cmd)
{
    struct ata_channel *c = d->ch;
    if (wait_not_busy(c) < 0)
        return -1;
    /* Drive register: bit 6 = LBA mode, bit 4 = slave, bits 0-3 = LBA 24-27.
       Bits 7 and 5 must be 1 on old drives. */
    outb(c->base + REG_DRIVE, 0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    delay400ns(c);
    outb(c->base + REG_SECCOUNT, count);    /* (256 would be written as 0) */
    outb(c->base + REG_LBA_LO,  lba & 0xFF);
    outb(c->base + REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(c->base + REG_LBA_HI,  (lba >> 16) & 0xFF);
    c->irq_seen = 0;                        /* only this command's IRQs count */
    outb(c->base + REG_COMMAND, cmd);
    return 0;
}

/* Read count sectors: the drive interrupts once per block (d->multi
   sectors) when the block is in its buffer; then we copy it out. */
static int pio_read(struct ata_drive *d, uint32_t lba, uint32_t count, uint8_t *p,
                    uint32_t timeout_ms)
{
    struct ata_channel *c = d->ch;
    uint8_t cmd = d->multi > 1 ? CMD_READ_MULTIPLE : CMD_READ_SECTORS;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        if (start(d, lba, n, cmd) < 0)
            return -1;
        for (uint32_t done = 0; done < n; ) {
            int st = wait_drive(c, timeout_ms);
            if (bad(st) || !(st & ST_DRQ))
                return -1;
            uint32_t k = n - done < d->multi ? n - done : d->multi;
            insw(c->base + REG_DATA, p, k * SECTOR_SIZE / 2);
            p += k * SECTOR_SIZE;
            done += k;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_drive *d = dev->priv;
    uint32_t flags = irq_save();
    channel_take(d->ch);
    int err = pio_read(d, lba, count, buf, IRQ_TIMEOUT);
    channel_give(d->ch);
    irq_restore(flags);
    return err;
}

/* Write count sectors: the drive asks for the first block straight away
   (no interrupt), then interrupts after each: "next one, please" — and
   after the last: "done". The data may still sit in the drive's own
   cache; see ata_flush. */
static int ata_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    struct ata_drive *d = dev->priv;
    struct ata_channel *c = d->ch;
    const uint8_t *p = buf;
    uint8_t cmd = d->multi > 1 ? CMD_WRITE_MULTIPLE : CMD_WRITE_SECTORS;
    int err = 0;
    uint32_t flags = irq_save();
    channel_take(c);
    while (count && !err) {
        uint32_t n = count > 128 ? 128 : count;
        err = start(d, lba, n, cmd);
        for (uint32_t done = 0; done < n && !err; ) {
            int st;
            if (done == 0) {
                delay400ns(c);
                st = wait_not_busy(c);
            } else {
                st = wait_drive(c, IRQ_TIMEOUT);
            }
            if (bad(st) || !(st & ST_DRQ)) {
                err = -1;
                break;
            }
            uint32_t k = n - done < d->multi ? n - done : d->multi;
            outsw(c->base + REG_DATA, p, k * SECTOR_SIZE / 2);
            p += k * SECTOR_SIZE;
            done += k;
        }
        if (!err && bad(wait_drive(c, IRQ_TIMEOUT)))   /* "done" */
            err = -1;
        lba += n;
        count -= n;
    }
    channel_give(c);
    irq_restore(flags);
    return err;
}

/* Drives keep written data in a cache of their own; this makes them put
   it on the platters. Done once after a batch of writes (it can take a
   while), not after every one. */
static int ata_flush(struct blockdev *dev)
{
    struct ata_drive *d = dev->priv;
    struct ata_channel *c = d->ch;
    uint32_t flags = irq_save();
    channel_take(c);
    int err = -1;
    if (wait_not_busy(c) >= 0) {
        outb(c->base + REG_DRIVE, 0xE0 | (d->slave << 4));
        delay400ns(c);
        c->irq_seen = 0;
        outb(c->base + REG_COMMAND, CMD_CACHE_FLUSH);
        err = bad(wait_drive(c, FLUSH_TIMEOUT)) ? -1 : 0;
    }
    channel_give(c);
    irq_restore(flags);
    return err;
}

/* --- finding the drives ------------------------------------------------------ */

/*
 * IDENTIFY: ask a drive to describe itself. It answers with 256 16-bit words.
 * Useful ones:  words 27-46 = model name, words 60-61 = LBA28 sector count.
 * Returns 0 if an ATA hard disk answered, -1 otherwise. (Polled: the
 * channel's interrupt is off while we look.)
 */
static int identify(struct ata_drive *d, uint16_t id[256])
{
    struct ata_channel *c = d->ch;
    outb(c->base + REG_DRIVE, 0xA0 | (d->slave << 4));
    delay400ns(c);
    outb(c->base + REG_SECCOUNT, 0);
    outb(c->base + REG_LBA_LO, 0);
    outb(c->base + REG_LBA_MID, 0);
    outb(c->base + REG_LBA_HI, 0);
    outb(c->base + REG_COMMAND, CMD_IDENTIFY);

    if (inb(c->base + REG_STATUS) == 0)
        return -1;                          /* status 0: no drive here */
    if (wait_not_busy(c) < 0)
        return -1;
    /* ATAPI (CD-ROM) and SATA devices put a signature in LBA_MID/HI and
       don't answer IDENTIFY. Not a hard disk — skip it. */
    if (inb(c->base + REG_LBA_MID) || inb(c->base + REG_LBA_HI))
        return -1;
    delay400ns(c);
    int st = wait_not_busy(c);
    if (bad(st) || !(st & ST_DRQ))
        return -1;
    insw(c->base + REG_DATA, id, 256);
    return 0;
}

/* The model string is stored with the two bytes of each word swapped. */
static void get_model(const uint16_t id[256], char *model /* [41] */)
{
    for (int i = 0; i < 20; i++) {
        model[2 * i]     = id[27 + i] >> 8;
        model[2 * i + 1] = id[27 + i] & 0xFF;
    }
    int len = 40;
    while (len > 0 && model[len - 1] == ' ')
        len--;                              /* trim trailing spaces */
    model[len] = '\0';
}

/* Ask for READ/WRITE MULTIPLE in blocks of MULTI_WANT sectors (or the
   drive's maximum, IDENTIFY word 47). Polled, at boot. */
static void set_multiple(struct ata_drive *d, const uint16_t id[256])
{
    struct ata_channel *c = d->ch;
    uint32_t max = id[47] & 0xFF, want = MULTI_WANT;
    d->multi = 1;
    while (want > max)
        want /= 2;                  /* a power of two the drive allows */
    if (want < 2)
        return;
    outb(c->base + REG_DRIVE, 0xE0 | (d->slave << 4));
    delay400ns(c);
    outb(c->base + REG_SECCOUNT, want);
    outb(c->base + REG_COMMAND, CMD_SET_MULTIPLE);
    delay400ns(c);
    if (!bad(wait_not_busy(c)))
        d->multi = want;
}

/* Turn the channel's interrupt on, and check that it really arrives by
   reading one sector. If it doesn't, poll this channel from now on. */
static void enable_irq(struct ata_channel *c, struct ata_drive *d)
{
    static uint8_t sector[SECTOR_SIZE];
    irq_install(c->irq, c->irq == 14 ? irq14 : irq15);
    uint32_t flags = irq_save();
    inb(c->base + REG_STATUS);              /* forget anything pending */
    outb(c->ctrl, 0);                       /* nIEN off: interrupts on */
    c->use_irq = 1;
    if (pio_read(d, 0, 1, sector, PROBE_TIMEOUT) < 0) {
        c->use_irq = 0;
        outb(c->ctrl, CTL_NIEN);
        kprintf("ata: IRQ %lu never arrived: polling this channel\n", (uint32_t)c->irq);
    }
    irq_restore(flags);
}

void ata_init(void)
{
    static const uint16_t bases[2] = { 0x1F0, 0x170 };
    static const uint16_t ctrls[2] = { 0x3F6, 0x376 };
    static uint16_t id[256];

    for (int ch = 0; ch < 2; ch++) {
        struct ata_channel *c = &channels[ch];
        c->base = bases[ch];
        c->ctrl = ctrls[ch];
        c->irq = 14 + ch;
        /* A status of 0xFF means nothing is connected ("floating bus"). */
        if (inb(c->base + REG_STATUS) == 0xFF)
            continue;
        outb(c->ctrl, CTL_NIEN);            /* poll while we look */

        struct ata_drive *first = NULL;
        for (int sl = 0; sl < 2; sl++) {
            struct ata_drive *d = &drives[ch * 2 + sl];
            d->ch = c;
            d->slave = sl;

            if (identify(d, id) < 0)
                continue;
            uint32_t sectors = id[60] | ((uint32_t)id[61] << 16);
            if (sectors == 0)
                continue;                   /* drive doesn't support LBA */

            struct blockdev bd = {
                .name    = { 'h', 'd', 'a' + ch * 2 + sl, '\0' },
                .sectors = sectors,
                .read    = ata_read,
                .write   = ata_write,
                .flush   = ata_flush,
                .priv    = d,
            };
            char model[41];
            get_model(id, model);
            set_multiple(d, id);
            if (blockdev_register(&bd))
                kprintf("ata: %s: %lu sectors (%lu MiB) \"%s\", %lu sectors per IRQ\n",
                        bd.name, sectors, sectors / 2048, model, (uint32_t)d->multi);
            if (!first)
                first = d;
        }
        if (first)
            enable_irq(c, first);
    }
}
