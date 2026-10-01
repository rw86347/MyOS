/*
 * partition.c — read the MBR partition table and register each partition
 * as its own block device (hdb -> hdb1, hdb2, ...), like Linux does.
 *
 * The table is 4 entries x 16 bytes at offset 446 of sector 0, followed by
 * the 0x55 0xAA signature. (tools/mkusb.sh writes one; see its comments
 * for the byte layout.) Extended/logical partitions and GPT come later.
 */
#include <blockdev.h>
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
    default:   return "unknown";
    }
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
        if (e.type == 0x05 || e.type == 0x0F || e.type == 0xEE) {
            kprintf("part: %s: entry %d is %s (type %02x) - not supported yet\n",
                    disk->name, i + 1, e.type == 0xEE ? "GPT" : "extended", e.type);
            continue;
        }
        if (e.lba_first >= disk->sectors || e.sectors > disk->sectors - e.lba_first) {
            kprintf("part: %s: entry %d runs past end of disk - ignored\n",
                    disk->name, i + 1);
            continue;
        }

        struct blockdev part = {
            .sectors = e.sectors,
            .parent  = disk,
            .offset  = e.lba_first,
            .priv    = (void *)(uintptr_t)e.type,   /* remember the type byte */
        };
        size_t len = strlen(disk->name);
        memcpy(part.name, disk->name, len);         /* "hdb" + "1" */
        part.name[len] = '1' + i;
        part.name[len + 1] = '\0';

        if (blockdev_register(&part)) {
            kprintf("part: %s: start %lu, %lu sectors (%lu MiB), type %02x %s\n",
                    part.name, e.lba_first, e.sectors, e.sectors / 2048,
                    e.type, type_name(e.type));
            found++;
        }
    }
    return found;
}
