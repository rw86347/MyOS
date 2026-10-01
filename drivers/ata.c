/*
 * ata.c — ATA (IDE) hard disk driver, PIO mode, polling
 *
 * Every PC from ~1990 to ~2008 has IDE (also called PATA) disk controllers,
 * and later SATA chipsets can emulate them ("IDE/legacy mode" in the BIOS).
 * QEMU's default PC has one too, so this driver is our first disk driver.
 *
 * A PC has two IDE "channels", each with up to two drives (master/slave):
 *
 *   channel     command ports   control port   Linux name
 *   primary     0x1F0-0x1F7     0x3F6          hda (master), hdb (slave)
 *   secondary   0x170-0x177     0x376          hdc (master), hdd (slave)
 *
 * "PIO" = Programmed I/O: the CPU copies every 16-bit word between the disk
 * and memory itself (rep insw / rep outsw). Simple, slow (~a few MB/s) and
 * always works. DMA (the disk writes to RAM by itself) comes later.
 *
 * "Polling" = we spin reading the status register instead of waiting for an
 * interrupt (IRQ 14/15). Simpler; switching to IRQs is in docs/gaps.md.
 *
 * Addressing: LBA28 — sector numbers up to 2^28 (128 GiB). Enough for now;
 * LBA48 (bigger disks) is a small addition later.
 */
#include <blockdev.h>
#include <io.h>
#include <kprintf.h>
#include <ata.h>

/* Registers, as offsets from the channel's command base port. */
#define REG_DATA      0   /* 16-bit data in/out */
#define REG_ERROR     1   /* read: error details */
#define REG_SECCOUNT  2   /* number of sectors to transfer */
#define REG_LBA_LO    3   /* LBA bits 0-7 */
#define REG_LBA_MID   4   /* LBA bits 8-15 */
#define REG_LBA_HI    5   /* LBA bits 16-23 */
#define REG_DRIVE     6   /* drive select + LBA bits 24-27 */
#define REG_STATUS    7   /* read: status */
#define REG_COMMAND   7   /* write: command */

/* Status register bits */
#define ST_ERR  0x01      /* the last command failed */
#define ST_DRQ  0x08      /* drive is ready to transfer a sector of data */
#define ST_DF   0x20      /* drive fault */
#define ST_BSY  0x80      /* drive is busy: ignore all other bits */

/* Commands */
#define CMD_READ_SECTORS   0x20
#define CMD_WRITE_SECTORS  0x30
#define CMD_CACHE_FLUSH    0xE7
#define CMD_IDENTIFY       0xEC

/* Device control register (the control port) */
#define CTL_NIEN  0x02    /* "no interrupts": we poll instead */

/* How many status reads before giving up (~1 second on real hardware). */
#define TIMEOUT 1000000

struct ata_drive {
    uint16_t base;        /* command block base port, e.g. 0x1F0 */
    uint16_t ctrl;        /* control port, e.g. 0x3F6 */
    uint8_t  slave;       /* 0 = master, 1 = slave */
};

static struct ata_drive drives[4];

/*
 * After selecting a drive, the spec says wait 400 ns before trusting the
 * status register. Each port read takes ~100 ns, so read the "alternate
 * status" register (same value, but reading it has no side effects) 4 times.
 */
static void delay400ns(struct ata_drive *d)
{
    for (int i = 0; i < 4; i++)
        inb(d->ctrl);
}

/* Wait until BSY clears. Returns the final status, or -1 on timeout. */
static int wait_not_busy(struct ata_drive *d)
{
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t st = inb(d->base + REG_STATUS);
        if (!(st & ST_BSY))
            return st;
    }
    return -1;
}

/* Wait until the drive is ready to transfer data (DRQ), or reports an error. */
static int wait_drq(struct ata_drive *d)
{
    delay400ns(d);
    int st = wait_not_busy(d);
    if (st < 0 || (st & (ST_ERR | ST_DF)) || !(st & ST_DRQ))
        return -1;
    return 0;
}

/* Select the drive and load the LBA28 address + sector count registers. */
static void setup_transfer(struct ata_drive *d, uint32_t lba, uint8_t count)
{
    /* Drive register: bit 6 = LBA mode, bit 4 = slave, bits 0-3 = LBA 24-27.
       Bits 7 and 5 must be 1 on old drives. */
    outb(d->base + REG_DRIVE, 0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    delay400ns(d);
    outb(d->base + REG_SECCOUNT, count);
    outb(d->base + REG_LBA_LO,  lba & 0xFF);
    outb(d->base + REG_LBA_MID, (lba >> 8) & 0xFF);
    outb(d->base + REG_LBA_HI,  (lba >> 16) & 0xFF);
}

static int ata_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_drive *d = dev->priv;
    uint8_t *p = buf;

    while (count) {
        /* One command can move up to 256 sectors (count register 0 = 256);
           we use chunks of 128 to keep it simple. */
        uint8_t n = count > 128 ? 128 : count;

        if (wait_not_busy(d) < 0)
            return -1;
        setup_transfer(d, lba, n);
        outb(d->base + REG_COMMAND, CMD_READ_SECTORS);

        for (int i = 0; i < n; i++) {       /* the drive signals DRQ per sector */
            if (wait_drq(d) < 0)
                return -1;
            insw(d->base + REG_DATA, p, SECTOR_SIZE / 2);
            p += SECTOR_SIZE;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    struct ata_drive *d = dev->priv;
    const uint8_t *p = buf;

    while (count) {
        uint8_t n = count > 128 ? 128 : count;

        if (wait_not_busy(d) < 0)
            return -1;
        setup_transfer(d, lba, n);
        outb(d->base + REG_COMMAND, CMD_WRITE_SECTORS);

        for (int i = 0; i < n; i++) {
            if (wait_drq(d) < 0)
                return -1;
            outsw(d->base + REG_DATA, p, SECTOR_SIZE / 2);
            p += SECTOR_SIZE;
        }
        lba += n;
        count -= n;
    }

    /* Drives keep written data in an internal cache; tell it to write that
       cache to the platters before we report success. */
    outb(d->base + REG_COMMAND, CMD_CACHE_FLUSH);
    int st = wait_not_busy(d);
    return (st < 0 || (st & (ST_ERR | ST_DF))) ? -1 : 0;
}

/*
 * IDENTIFY: ask a drive to describe itself. It answers with 256 16-bit words.
 * Useful ones:  words 27-46 = model name, words 60-61 = LBA28 sector count.
 * Returns 0 if an ATA hard disk answered, -1 otherwise.
 */
static int identify(struct ata_drive *d, uint16_t id[256])
{
    outb(d->base + REG_DRIVE, 0xA0 | (d->slave << 4));
    delay400ns(d);
    outb(d->base + REG_SECCOUNT, 0);
    outb(d->base + REG_LBA_LO, 0);
    outb(d->base + REG_LBA_MID, 0);
    outb(d->base + REG_LBA_HI, 0);
    outb(d->base + REG_COMMAND, CMD_IDENTIFY);

    if (inb(d->base + REG_STATUS) == 0)
        return -1;                          /* status 0: no drive here */
    if (wait_not_busy(d) < 0)
        return -1;
    /* ATAPI (CD-ROM) and SATA devices put a signature in LBA_MID/HI and
       don't answer IDENTIFY. Not a hard disk — skip it. */
    if (inb(d->base + REG_LBA_MID) || inb(d->base + REG_LBA_HI))
        return -1;
    if (wait_drq(d) < 0)
        return -1;
    insw(d->base + REG_DATA, id, 256);
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

void ata_init(void)
{
    static const uint16_t bases[2] = { 0x1F0, 0x170 };
    static const uint16_t ctrls[2] = { 0x3F6, 0x376 };
    static uint16_t id[256];

    for (int ch = 0; ch < 2; ch++) {
        /* A status of 0xFF means nothing is connected ("floating bus"). */
        if (inb(bases[ch] + REG_STATUS) == 0xFF)
            continue;
        outb(ctrls[ch], CTL_NIEN);          /* we poll; no interrupts please */

        for (int sl = 0; sl < 2; sl++) {
            struct ata_drive *d = &drives[ch * 2 + sl];
            d->base = bases[ch];
            d->ctrl = ctrls[ch];
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
                .priv    = d,
            };
            char model[41];
            get_model(id, model);
            if (blockdev_register(&bd))
                kprintf("ata: %s: %lu sectors (%lu MiB) \"%s\"\n",
                        bd.name, sectors, sectors / 2048, model);
        }
    }
}
