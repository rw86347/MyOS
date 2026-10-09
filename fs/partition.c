/*
 * partition.c — read the MBR partition table and register each partition
 * as its own block device (hdb -> hdb1, hdb2, ...), like Linux does.
 *
 * The table is 4 entries x 16 bytes at offset 446 of sector 0, followed by
 * the 0x55 0xAA signature. (tools/mkusb.sh writes one; see its comments
 * for the byte layout.) Type 0xEE means a GPT disk: the real table is at
 * LBA 1. Extended/logical MBR partitions still come later.
 */
#include <bcache.h>
#include <blockdev.h>
#include <errno.h>
#include <kprintf.h>
#include <partition.h>
#include <string.h>

struct mbr_entry {
    uint8_t  status;        /* 0x80 = bootable */
    uint8_t  chs_first[3];  /* old cylinder/head/sector form — ignored */
    uint8_t  type;          /* what's inside, e.g. 0x0C = FAT32 */
    uint8_t  chs_last[3];
    uint32_t lba_first;     /* first sector, counted from the start of the disk */
    uint32_t sectors;       /* length in sectors */
} __attribute__((packed));  /* "packed": no padding, match the disk layout exactly */

static const char *type_name(uint8_t type)
{
    switch (type) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    case 0xDA: return "other (GPT)";
    default:   return "unknown";
    }
}

/* A partition's name, as Linux makes it: the disk's name and the number,
   with a 'p' between them when the disk's name ends in a digit ("hdb" ->
   "hdb1", "nvme0n1" -> "nvme0n1p1"). 0 if it doesn't fit. */
static int part_name(char *out, size_t size, const char *disk, int n)
{
    size_t len = strlen(disk);
    int p = len && disk[len - 1] >= '0' && disk[len - 1] <= '9';
    if (n < 1 || n > 9 || len + p + 2 > size)
        return 0;
    memcpy(out, disk, len);
    if (p)
        out[len++] = 'p';
    out[len] = (char)('0' + n);
    out[len + 1] = '\0';
    return 1;
}

/* GPT type GUIDs as stored on disk (first three fields little-endian). */
static const uint8_t guid_esp[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
static const uint8_t guid_linux[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
static const uint8_t guid_msdata[16] = {        /* Microsoft basic data: NTFS, exFAT, FAT */
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

/* A GPT entry's name: 36 UTF-16 units at byte 56. Plain ASCII is kept,
   anything else becomes '?'. */
static void gpt_name(const uint8_t *e, char out[37])
{
    int k;
    for (k = 0; k < 36; k++) {
        uint16_t c = (uint16_t)(e[56 + 2 * k] | e[57 + 2 * k] << 8);
        if (!c)
            break;
        out[k] = c >= 32 && c < 127 ? (char)c : '?';
    }
    out[k] = 0;
}

/* BLKRRPART (partition_rescan): the disk whose table is being read again,
   and which slots it still has. Its partitions are updated in place — the
   devices array never shrinks, and an open /dev/sda2 or a cached block
   keeps pointing at the same struct — and a slot that is gone gets 0
   sectors, so every read or write of it fails. */
static struct blockdev *rescan;
static uint8_t seen[256 / 8];

static struct blockdev *part_of(struct blockdev *disk, int n)
{
    for (int k = 0; k < blockdev_count(); k++) {
        struct blockdev *d = blockdev_get(k);
        if (d->parent == disk && d->partno == n)
            return d;
    }
    return NULL;
}

static int add_part(struct blockdev *disk, int n, uint32_t lba, uint32_t sectors, uint8_t type,
                    const char *label)
{
    struct blockdev *old = rescan == disk ? part_of(disk, n) : NULL;
    if (rescan == disk)
        seen[n / 8] |= (uint8_t)(1u << (n % 8));
    if (old) {
        old->offset = lba;
        old->sectors = sectors;
        old->priv = (void *)(uintptr_t)type;
        strlcpy(old->label, label ? label : "", sizeof old->label);
        kprintf("part: %s: start %lu, %lu sectors (%lu MiB), type %02x %s\n",
                old->name, (unsigned long)lba, (unsigned long)sectors,
                (unsigned long)(sectors / 2048), type, type_name(type));
        return 1;
    }
    struct blockdev part = {
        .sectors = sectors,
        .parent  = disk,
        .offset  = lba,
        .priv    = (void *)(uintptr_t)type,
        .partno  = (uint8_t)n,
    };
    if (label)
        strlcpy(part.label, label, sizeof part.label);
    if (!part_name(part.name, sizeof part.name, disk->name, n) || !blockdev_register(&part))
        return 0;
    kprintf("part: %s: start %lu, %lu sectors (%lu MiB), type %02x %s\n",
            part.name, (unsigned long)lba, (unsigned long)sectors,
            (unsigned long)(sectors / 2048), type, type_name(type));
    return 1;
}

/* A type-0xEE MBR means the real table is GPT at LBA 1. */
static int gpt_scan(struct blockdev *disk)
{
    static uint8_t hdr[SECTOR_SIZE], ent[SECTOR_SIZE];
    uint32_t nents, entsz, i, found = 0;
    uint64_t elba;

    if (blockdev_read(disk, 1, 1, hdr) < 0)
        return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0) {
        kprintf("part: %s: protective MBR but no GPT header\n", disk->name);
        return 0;
    }
    memcpy(&elba, hdr + 72, 8);
    memcpy(&nents, hdr + 80, 4);
    memcpy(&entsz, hdr + 84, 4);
    if (entsz != 128 || !nents || elba > 0xFFFFFFFFull)
        return 0;
    if (nents > 16)
        nents = 16;                             /* we only need ESP + root */

    for (i = 0; i < nents; i++) {
        uint32_t slba, lba64_lo, last_lo;
        uint64_t first, last;
        uint8_t type;
        uint32_t sec = (uint32_t)elba + (i * 128) / SECTOR_SIZE;
        uint32_t off = (i * 128) % SECTOR_SIZE;
        if (off == 0 && blockdev_read(disk, sec, 1, ent) < 0)
            return (int)found;
        if (memcmp(ent + off, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0)
            continue;
        memcpy(&first, ent + off + 32, 8);
        memcpy(&last, ent + off + 40, 8);
        if (first > 0xFFFFFFFFull || last < first || last - first + 1 > 0xFFFFFFFFull)
            continue;
        slba = (uint32_t)first;
        if (slba >= disk->sectors)
            continue;
        /* Every used entry is a partition, numbered by its slot as on
           Linux (an installer formats /dev/sda3 meaning the third slot:
           counting only the ones we can mount would name another). Only
           0x83 and 0xEF are ever mounted; the type says which these are. */
        if (!memcmp(ent + off, guid_linux, 16))
            type = 0x83;
        else if (!memcmp(ent + off, guid_esp, 16))
            type = 0xEF;
        else if (!memcmp(ent + off, guid_msdata, 16))
            type = 0x07;
        else
            type = 0xDA;                        /* other data */
        char label[37];
        gpt_name(ent + off, label);
        last_lo = (last > 0xFFFFFFFFull) ? 0xFFFFFFFF : (uint32_t)last;
        lba64_lo = last_lo - slba + 1;
        if (lba64_lo > disk->sectors - slba)
            lba64_lo = disk->sectors - slba;
        found += add_part(disk, (int)i + 1, slba, lba64_lo, type, label);
    }
    return (int)found;
}

int partition_scan(struct blockdev *disk)
{
    static uint8_t sector[SECTOR_SIZE];
    int found = 0;

    if (blockdev_read(disk, 0, 1, sector) < 0)
        return -1;
    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return 0;                       /* no MBR: not partitioned */

    for (int i = 0; i < 4; i++) {
        struct mbr_entry e;
        memcpy(&e, sector + 446 + i * 16, sizeof e);

        if (e.type == 0 || e.sectors == 0)
            continue;                   /* empty slot */
        if (e.type == 0xEE)
            return gpt_scan(disk);
        if (e.type == 0x05 || e.type == 0x0F) {
            kprintf("part: %s: entry %d is extended (type %02x) - not supported yet\n",
                    disk->name, i + 1, e.type);
            continue;
        }
        if (e.lba_first >= disk->sectors || e.sectors > disk->sectors - e.lba_first) {
            kprintf("part: %s: entry %d runs past end of disk - ignored\n",
                    disk->name, i + 1);
            continue;
        }

        found += add_part(disk, i + 1, e.lba_first, e.sectors, e.type, NULL);
    }
    return found;
}

/* BLKRRPART: read disk's table again (sfdisk wrote a new one). The caller
   made sure nothing on the disk is mounted. Afterwards /sys/block and
   /dev show the new partitions, and the block cache has forgotten the
   disk — a cached block of the old sda2 is not one of the new sda2. */
int partition_rescan(struct blockdev *disk)
{
    if (disk->parent || rescan)
        return -EINVAL;                 /* partitions have no table; one at a time */
    rescan = disk;
    memset(seen, 0, sizeof seen);
    int found = partition_scan(disk);
    rescan = NULL;
    for (int k = 0; k < blockdev_count(); k++) {
        struct blockdev *d = blockdev_get(k);
        if (d->parent != disk)
            continue;
        if (found < 0 || !(seen[d->partno / 8] & (1u << (d->partno % 8))))
            d->sectors = 0;             /* gone */
        blockdev_changed(d);            /* /sys/block and /dev catch up */
    }
    bcache_drop(disk);
    return found < 0 ? -EIO : 0;
}
