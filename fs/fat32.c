/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fat32.c — FAT32 file system driver (read-only for now)
 *
 * FAT32 in one page
 * -----------------
 * The partition is divided into:
 *
 *   [reserved sectors][FAT 1][FAT 2][data area: cluster 2, 3, 4, ...]
 *
 * - The first reserved sector is the BOOT SECTOR. Its "BIOS Parameter Block"
 *   (BPB) describes the layout: bytes per sector, sectors per cluster, how
 *   many reserved sectors, how big each FAT is, which cluster the root
 *   directory starts in.
 * - The data area is split into CLUSTERS (1 or more sectors). Every file and
 *   directory is stored in a chain of clusters. Numbering starts at 2.
 * - The FAT (File Allocation Table) is an array with one 32-bit entry per
 *   cluster. Entry N says which cluster comes after cluster N in its file:
 *       0             free
 *       2..           next cluster in the chain
 *       >= 0x0FFFFFF8 end of chain (this was the file's last cluster)
 *   So a file is a linked list, and the FAT holds the "next" pointers.
 *   (Only the low 28 bits are used; FAT32 is really "FAT28".) There are
 *   usually two copies of the FAT in case one gets damaged.
 * - A DIRECTORY is just a file whose content is an array of 32-byte entries:
 *       0-10  name in 8.3 form ("NOTES   TXT"), space-padded
 *       11    attributes (directory, hidden, ...)
 *       12    case flags (Windows NT: base/extension are lowercase)
 *       20-21 first cluster, high 16 bits
 *       26-27 first cluster, low 16 bits
 *       28-31 file size in bytes
 *   First name byte 0x00 = end of directory, 0xE5 = deleted entry.
 * - LONG FILE NAMES (VFAT): names that don't fit 8.3 are stored in extra
 *   entries placed just BEFORE the 8.3 entry, with attribute 0x0F (which old
 *   DOS ignores). Each holds 13 UTF-16 characters, stored last-part-first.
 */
#include <fat32.h>
#include <interrupts.h>
#include <kprintf.h>
#include <string.h>

/* Sectors in one disk command when a file's clusters lie one after another
   on the disk (the usual case: files are written in one piece). 64 KiB,
   like a disk file's chunk (fs/dev.c). */
#define RUN_SECTORS   128

#define FAT_EOC_MIN   0x0FFFFFF8u  /* end-of-chain marker (any value >= this) */
#define FAT_BAD       0x0FFFFFF7u  /* cluster marked unusable */
#define FAT_MASK      0x0FFFFFFFu
#define DIRENT_SIZE   32

/* Read little-endian numbers from a byte buffer (disk formats are LE). */
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int valid_cluster(struct fat32_fs *fs, uint32_t c)
{
    return c >= 2 && c < fs->total_clusters + 2;
}

/* First sector (within the partition) of a data cluster. */
static uint32_t cluster_to_sector(struct fat32_fs *fs, uint32_t c)
{
    return fs->first_data_sector + (c - 2) * fs->sectors_per_cluster;
}

/* Look up cluster c in the FAT: returns the next cluster in the chain, a
   value >= FAT_EOC_MIN at the end, or 0 on error. Caches one FAT sector,
   because walking a chain usually reads neighbouring entries.
   Two threads may walk chains at once (the kernel preempts), and the read
   sleeps: the sector is read into a buffer of our own, and the cache is
   only looked at or changed with interrupts off, so nobody ever sees a
   sector number with another sector's bytes. */
static uint32_t next_cluster(struct fat32_fs *fs, uint32_t c)
{
    uint32_t byte_offset = c * 4;
    uint32_t sector = fs->reserved_sectors + byte_offset / SECTOR_SIZE;
    uint32_t off = byte_offset % SECTOR_SIZE, next;
    uint8_t buf[SECTOR_SIZE];

    unsigned long f = irq_save();
    int hit = sector == fs->fat_cache_sector;
    next = hit ? rd32(fs->fat_cache + off) : 0;
    irq_restore(f);
    if (hit)
        return next & FAT_MASK;
    if (blockdev_read(fs->dev, sector, 1, buf) < 0)
        return 0;
    f = irq_save();
    memcpy(fs->fat_cache, buf, SECTOR_SIZE);
    fs->fat_cache_sector = sector;
    irq_restore(f);
    return rd32(buf + off) & FAT_MASK;
}

/* The cluster with index `index` in the chain that starts at `first`.
   The walk starts where the last read of the same file ended, if that is
   not past `index`: a file read front to back walks its chain once in all.
   (It walked from the start for every read, and read() came in 512-byte
   pieces: copying a 240 MB file off a stick took about an hour.) The FAT
   only reads, so a remembered place in a chain stays true. */
static uint32_t seek_cluster(struct fat32_fs *fs, uint32_t first, uint32_t index)
{
    uint32_t c = first, at = 0;
    unsigned long f = irq_save();
    if (fs->pos_first == first && fs->pos_index <= index) {
        c = fs->pos_cluster;
        at = fs->pos_index;
    }
    irq_restore(f);
    for (; at < index; at++) {
        c = next_cluster(fs, c);
        if (!valid_cluster(fs, c))
            return 0;
    }
    return c;
}

static void remember_cluster(struct fat32_fs *fs, uint32_t first, uint32_t index, uint32_t c)
{
    unsigned long f = irq_save();
    fs->pos_first = first;
    fs->pos_index = index;
    fs->pos_cluster = c;
    irq_restore(f);
}

int fat32_mount(struct fat32_fs *fs, struct blockdev *dev)
{
    uint8_t bs[SECTOR_SIZE];        /* the boot sector */

    memset(fs, 0, sizeof *fs);
    fs->dev = dev;
    fs->fat_cache_sector = 0xFFFFFFFF;  /* cache empty */

    if (blockdev_read(dev, 0, 1, bs) < 0)
        return -1;
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        return -1;

    uint32_t bytes_per_sector = rd16(bs + 11);
    fs->sectors_per_cluster   = bs[13];
    fs->reserved_sectors      = rd16(bs + 14);
    fs->num_fats              = bs[16];
    uint32_t root_entries     = rd16(bs + 17);  /* FAT12/16 only; 0 on FAT32 */
    uint32_t total_sectors    = rd16(bs + 19);  /* "small" count; 0 if large */
    uint32_t fat_size_16      = rd16(bs + 22);  /* FAT12/16 only; 0 on FAT32 */
    if (total_sectors == 0)
        total_sectors = rd32(bs + 32);
    fs->fat_size     = rd32(bs + 36);
    fs->root_cluster = rd32(bs + 44);

    /* Sanity checks — a random sector should never pass all of these. */
    if (bytes_per_sector != SECTOR_SIZE)
        return -1;                      /* we only support 512-byte sectors */
    uint32_t spc = fs->sectors_per_cluster;
    if (spc == 0 || (spc & (spc - 1)) != 0)
        return -1;                      /* must be a power of two */
    if (fs->num_fats == 0 || fs->reserved_sectors == 0)
        return -1;
    if (root_entries != 0 || fat_size_16 != 0 || fs->fat_size == 0)
        return -1;                      /* those mean FAT12/16, not FAT32 */

    fs->first_data_sector = fs->reserved_sectors + fs->num_fats * fs->fat_size;
    if (total_sectors <= fs->first_data_sector || total_sectors > dev->sectors)
        return -1;
    fs->total_clusters = (total_sectors - fs->first_data_sector) / spc;

    /* The official rule: the cluster COUNT decides FAT12/16/32, not the
       label. Under 65525 clusters it is not FAT32. */
    if (fs->total_clusters < 65525 || !valid_cluster(fs, fs->root_cluster))
        return -1;

    /* Volume label: 11 space-padded bytes at offset 71. */
    memcpy(fs->label, bs + 71, 11);
    int n = 11;
    while (n > 0 && fs->label[n - 1] == ' ')
        n--;
    fs->label[n] = '\0';
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Directory reading                                                         */
/* ------------------------------------------------------------------------ */

/* Checksum of an 8.3 name; every LFN entry stores it so we can tell the
   long name really belongs to the 8.3 entry that follows it. */
static uint8_t lfn_checksum(const uint8_t *short_name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

/* Turn "NOTES   TXT" into "NOTES.TXT" (or "notes.txt" if the case flags say so). */
static void short_name(const uint8_t *e, char *out)
{
    uint8_t flags = e[12];
    int n = 0;

    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (i == 0 && e[0] == 0x05) ? (char)0xE5 : (char)e[i]; /* 0x05 escapes 0xE5 */
        if ((flags & 0x08) && c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        out[n++] = c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            if ((flags & 0x10) && c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

/* State for collecting a long name from its LFN entries.
   Complete when next_seq has counted down to 0 and name[0] is set. */
struct lfn_state {
    char    name[256];
    int     next_seq;       /* sequence number we expect next; 0 = none in progress */
    uint8_t checksum;
};

/* Character positions inside an LFN entry: 13 UTF-16 chars in 3 pieces. */
static const uint8_t lfn_offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static void lfn_add(struct lfn_state *st, const uint8_t *e)
{
    int seq = e[0] & 0x1F;

    if (e[0] & 0x40) {              /* "last" flag: this is the first one we see */
        memset(st->name, 0, sizeof st->name);
        st->next_seq = seq;
        st->checksum = e[13];
    }
    if (seq == 0 || seq > 20 || seq != st->next_seq || e[13] != st->checksum) {
        st->next_seq = 0;           /* out of order or mismatched: give up */
        st->name[0] = '\0';         /* ...and make sure the partial name isn't used */
        return;
    }
    for (int i = 0; i < 13; i++) {
        uint16_t ch = rd16(e + lfn_offsets[i]);
        int pos = (seq - 1) * 13 + i;
        if (ch == 0x0000 || ch == 0xFFFF || pos >= 255)
            break;                  /* terminator / padding / too long */
        /* We only handle ASCII for now; other characters become '?'. */
        st->name[pos] = ch < 0x80 ? (char)ch : '?';
    }
    st->next_seq = seq - 1;         /* the next entry holds the previous part */
}

int fat32_readdir(struct fat32_fs *fs, const struct fat32_dirent *dir,
                  fat32_dir_cb cb, void *ctx)
{
    uint8_t buf[SECTOR_SIZE];       /* on the stack: safe for recursive callers */
    struct lfn_state lfn = { .next_seq = 0 };
    struct fat32_dirent ent;
    uint32_t c = dir->cluster;

    if (!(dir->attr & FAT_ATTR_DIRECTORY))
        return -1;

    /* Follow the directory's cluster chain. The step limit stops an endless
       loop if a damaged FAT links a chain back into itself. */
    for (uint32_t steps = 0; valid_cluster(fs, c) && steps < fs->total_clusters; steps++) {
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (blockdev_read(fs->dev, cluster_to_sector(fs, c) + s, 1, buf) < 0)
                return -1;

            for (int off = 0; off < SECTOR_SIZE; off += DIRENT_SIZE) {
                const uint8_t *e = buf + off;
                uint8_t attr = e[11];

                if (e[0] == 0x00)
                    return 0;           /* end of directory */
                if (e[0] == 0xE5) {     /* deleted */
                    lfn.next_seq = 0;
                    continue;
                }
                if ((attr & 0x3F) == FAT_ATTR_LFN) {
                    lfn_add(&lfn, e);
                    continue;
                }
                if (attr & FAT_ATTR_VOLUME_ID) {  /* the volume label entry */
                    lfn.next_seq = 0;
                    continue;
                }

                /* A normal 8.3 entry. Use the long name if one was completed
                   just before it and the checksum matches. */
                if (lfn.next_seq == 0 && lfn.name[0] && lfn.checksum == lfn_checksum(e))
                    memcpy(ent.name, lfn.name, sizeof ent.name);
                else
                    short_name(e, ent.name);
                memset(&lfn, 0, sizeof lfn);

                if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
                    continue;

                ent.attr    = attr;
                ent.cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                ent.size    = rd32(e + 28);

                int r = cb(&ent, ctx);
                if (r)
                    return r;
            }
        }
        c = next_cluster(fs, c);
        if (c == 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Path lookup                                                               */
/* ------------------------------------------------------------------------ */

struct find_ctx {
    const char *want;
    struct fat32_dirent *out;
};

static int find_cb(const struct fat32_dirent *e, void *ctx)
{
    struct find_ctx *f = ctx;
    if (strcasecmp(e->name, f->want) == 0) {
        *f->out = *e;
        return 1;                   /* found: stop */
    }
    return 0;
}

int fat32_lookup(struct fat32_fs *fs, const char *path, struct fat32_dirent *out)
{
    /* Start at the root directory. */
    memset(out, 0, sizeof *out);
    out->name[0] = '/';
    out->attr = FAT_ATTR_DIRECTORY;
    out->cluster = fs->root_cluster;

    while (*path) {
        while (*path == '/')
            path++;                 /* skip slashes */
        if (!*path)
            break;

        char part[256];             /* the next path component */
        int n = 0;
        while (*path && *path != '/' && n < 255)
            part[n++] = *path++;
        part[n] = '\0';

        if (!(out->attr & FAT_ATTR_DIRECTORY))
            return -1;              /* "file.txt/more" */
        struct fat32_dirent found;
        struct find_ctx f = { part, &found };
        if (fat32_readdir(fs, out, find_cb, &f) != 1)
            return -1;              /* not found (or disk error) */
        *out = found;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* File reading                                                              */
/* ------------------------------------------------------------------------ */

int fat32_read(struct fat32_fs *fs, const struct fat32_dirent *file,
               uint32_t offset, void *buf, uint32_t len)
{
    uint8_t sector[SECTOR_SIZE];
    uint8_t *out = buf;
    uint32_t spc = fs->sectors_per_cluster, cluster_bytes = spc * SECTOR_SIZE;

    if (file->attr & FAT_ATTR_DIRECTORY)
        return -1;
    if (offset >= file->size)
        return 0;
    if (len > file->size - offset)
        len = file->size - offset;  /* don't read past end of file */

    uint32_t index = offset / cluster_bytes;    /* which cluster of the file */
    uint32_t c = seek_cluster(fs, file->cluster, index);
    uint32_t done = 0;
    uint32_t pos = offset % cluster_bytes;      /* position inside cluster c */
    while (done < len) {
        if (!valid_cluster(fs, c))
            return -1;
        uint32_t lba = cluster_to_sector(fs, c) + pos / SECTOR_SIZE;
        uint32_t in_sector = pos % SECTOR_SIZE;
        if (in_sector || len - done < SECTOR_SIZE) {
            /* Part of one sector: read it whole, copy the part we need. */
            if (blockdev_read(fs->dev, lba, 1, sector) < 0)
                return -1;
            uint32_t n = SECTOR_SIZE - in_sector;
            if (n > len - done)
                n = len - done;
            memcpy(out + done, sector + in_sector, n);
            done += n;
            pos += n;
        } else {
            /* Whole sectors, straight into buf: the rest of this cluster
               and of the clusters right after it on the disk, in one
               command of up to RUN_SECTORS. */
            uint32_t want = (len - done) / SECTOR_SIZE, n = spc - pos / SECTOR_SIZE, last = c;
            if (want > RUN_SECTORS)
                want = RUN_SECTORS;
            while (n < want && next_cluster(fs, last) == last + 1 && valid_cluster(fs, last + 1)) {
                last++;
                n += spc;
            }
            if (n > want)
                n = want;
            if (blockdev_read(fs->dev, lba, n, out + done) < 0)
                return -1;
            done += n * SECTOR_SIZE;
            pos += n * SECTOR_SIZE;
            while (pos >= cluster_bytes && c < last) {  /* into the clusters read */
                c++;
                index++;
                pos -= cluster_bytes;
            }
        }
        if (pos == cluster_bytes && done < len) {   /* move to next cluster */
            c = next_cluster(fs, c);
            index++;
            pos = 0;
        }
    }
    remember_cluster(fs, file->cluster, index, c);
    return (int)done;
}
