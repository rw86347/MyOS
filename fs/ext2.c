/*
 * ext2.c — the second extended file system, Linux's classic disk format
 *
 * A disk formatted with ext2 (mke2fs) is cut into equal BLOCKS (1, 2 or
 * 4 KiB), and the blocks into GROUPS of a few thousand. The layout:
 *
 *   byte 1024   the superblock: block size, how many blocks and inodes,
 *               how many are free, which features the disk uses
 *   next block  the group descriptor table: per group, where its two
 *               bitmaps and its inode table are, and its free counts
 *   per group   a block bitmap (1 bit per block: used?), an inode bitmap,
 *               the inode table, then data blocks
 *
 * An INODE (128+ bytes) describes one file: type and permissions, owner,
 * size, times, link count, and where its data is — 15 block numbers:
 *
 *   block[0..11]  the first 12 data blocks, directly
 *   block[12]     an indirect block: a block full of block numbers
 *   block[13]     double indirect: block numbers of indirect blocks
 *   block[14]     triple indirect
 *
 * A DIRECTORY is a file whose data is a list of entries: inode number,
 * entry length, name length, file type, name. Inode 2 is the root
 * directory. A small symlink keeps its target inside block[] itself.
 *
 * How it fits in (see vfs.h): the VFS keeps the tree in memory; a disk
 * directory joins it the first time someone looks inside (load_dir).
 * File data goes through the block cache (bcache.c) — read() copies
 * from a cached block, write() changes one and marks it dirty; the cache
 * writes it to the disk within a second or so.
 *
 * No journal (that's ext3): switching off in the middle of a change can
 * leave the disk inconsistent; e2fsck repairs it. Not supported (refused
 * at mount): ext3/4 extents, 64-bit, flex_bg, compression; disks with
 * checksums or other features we can't keep up to date are mounted
 * read-only. Hard links can be read but not made.
 *
 * Locking: one operation at a time per file system (a sleeping lock, as
 * for the IDE channel), because operations sleep halfway on the disk.
 */
#include <bcache.h>
#include <errno.h>
#include <ext2.h>
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <string.h>
#include <thread.h>
#include <uaccess.h>
#include <vfs.h>

/* --- the on-disk format (little-endian, like the CPU) ------------------- */

struct ext2_super {                     /* the parts we use, at byte 1024 */
    uint32_t inodes_count, blocks_count, r_blocks_count;
    uint32_t free_blocks_count, free_inodes_count;
    uint32_t first_data_block, log_block_size, log_frag_size;
    uint32_t blocks_per_group, frags_per_group, inodes_per_group;
    uint32_t mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev_level;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;                 /* rev 1 from here */
    uint16_t inode_size, block_group_nr;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
} __attribute__((packed));

struct ext2_gd {                        /* one group's descriptor */
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks_count, free_inodes_count, used_dirs_count, pad;
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct ext2_gd) == 32, "ext2 group descriptor");

struct ext2_inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links_count;
    uint32_t blocks;                    /* in 512-byte units, all blocks used */
    uint32_t flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_high, faddr;
    uint16_t blocks_hi, file_acl_hi, uid_high, gid_high;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ext2_inode) == 128, "ext2 inode");

struct ext2_dirent {
    uint32_t inode;                     /* 0: an unused entry */
    uint16_t rec_len;                   /* to the next entry */
    uint8_t  name_len, file_type;
    char     name[];
};

#define EXT2_MAGIC          0xEF53
#define ROOT_INO            2
#define VALID_FS            1           /* superblock state: cleanly unmounted */
#define INCOMPAT_FILETYPE   0x0002      /* entries carry the file's type */
#define RO_COMPAT_SPARSE    0x0001      /* backup superblocks in fewer groups */
#define RO_COMPAT_LARGEFILE 0x0002      /* files over 2 GiB may exist */
#define INDEX_FL            0x1000      /* a directory with a hash index */
#define DIRENT_LEN(n)       ((8 + (n) + 3) & ~3u)
#define RW_MAX              (256 * 1024)    /* per read()/write(), see ramfs.c */

enum { FT_UNKNOWN, FT_REG, FT_DIR, FT_CHR, FT_BLK, FT_FIFO, FT_SOCK, FT_LNK };

/* --- one mounted ext2 file system ---------------------------------------- */

struct ext2 {
    struct mount m;                     /* what the VFS sees (must be first) */
    struct blockdev *dev;
    uint32_t bsize, ptrs;               /* block size; block numbers per block */
    uint32_t groups, ipg, bpg, isize;   /* inodes/blocks per group; inode size */
    uint32_t first_data, first_ino, blocks, inodes;
    uint32_t gd_block;                  /* the group descriptor table */
    int filetype, readonly;
    struct buf *sb_buf;                 /* the superblock: held while mounted */
    struct ext2_super *sb;
    uint8_t busy;                       /* the lock */
    struct wait_queue turn;
};

static struct ext2 *fs_of(struct inode *i) { return (struct ext2 *)i->mnt; }
static uint32_t min(uint32_t a, uint32_t b) { return a < b ? a : b; }

static uint32_t lock(struct ext2 *fs)
{
    uint32_t flags = irq_save();
    while (fs->busy)
        wq_sleep(&fs->turn);
    fs->busy = 1;
    return flags;
}

static void unlock(struct ext2 *fs, uint32_t flags)
{
    fs->busy = 0;
    wq_wake_all(&fs->turn);
    irq_restore(flags);
}

/* --- groups, inodes, bitmaps ---------------------------------------------- */

static struct buf *get_group(struct ext2 *fs, uint32_t g, struct ext2_gd **gd)
{
    uint32_t per = fs->bsize / sizeof(struct ext2_gd);
    struct buf *b = bread(fs->dev, fs->gd_block + g / per, fs->bsize);
    if (b)
        *gd = (struct ext2_gd *)b->data + g % per;
    return b;
}

/* The buffer holding disk inode `ino` (held: brelse it), and a pointer to
   the inode inside it. NULL on a bad number or an I/O error. */
static struct buf *get_inode(struct ext2 *fs, uint32_t ino, struct ext2_inode **di)
{
    if (ino == 0 || ino > fs->inodes)
        return NULL;
    uint32_t g = (ino - 1) / fs->ipg, off = (ino - 1) % fs->ipg * fs->isize;
    struct ext2_gd *gd;
    struct buf *gb = get_group(fs, g, &gd);
    if (!gb)
        return NULL;
    uint32_t table = gd->inode_table;
    brelse(gb);
    struct buf *b = bread(fs->dev, table + off / fs->bsize, fs->bsize);
    if (b)
        *di = (struct ext2_inode *)(b->data + off % fs->bsize);
    return b;
}

static uint32_t group_of(struct ext2 *fs, uint32_t ino) { return (ino - 1) / fs->ipg; }

/* Find a 0 among the first n bits of a bitmap, set it, return its index
   (-1: all taken). 32 bits per test. Bit i is bit i%8 of byte i/8 — on a
   little-endian CPU, that's simply bit i%32 of 32-bit word i/32. */
static int take_bit(uint8_t *map, uint32_t n)
{
    uint32_t *w = (uint32_t *)map;
    for (uint32_t k = 0; k * 32 < n; k++) {
        if (w[k] == 0xFFFFFFFF)
            continue;
        uint32_t bit = __builtin_ctz(~w[k]), idx = k * 32 + bit;
        if (idx >= n)
            return -1;
        w[k] |= 1u << bit;
        return idx;
    }
    return -1;
}

static int clear_bit(uint8_t *map, uint32_t i)     /* 1 if it was set */
{
    int was = (map[i / 8] >> (i % 8)) & 1;
    map[i / 8] &= ~(1u << (i % 8));
    return was;
}

/* A free block, near `group` if possible (data close to its inode means
   short seeks). 0: the disk is full. */
static uint32_t alloc_block(struct ext2 *fs, uint32_t group)
{
    for (uint32_t t = 0; t < fs->groups; t++) {
        uint32_t g = (group + t) % fs->groups;
        struct ext2_gd *gd;
        struct buf *gb = get_group(fs, g, &gd);
        if (!gb)
            return 0;
        int bit = -1;
        if (gd->free_blocks_count) {
            struct buf *bb = bread(fs->dev, gd->block_bitmap, fs->bsize);
            if (bb) {
                bit = take_bit(bb->data, min(fs->bpg, fs->blocks - fs->first_data - g * fs->bpg));
                if (bit >= 0) {
                    bdirty(bb);
                    gd->free_blocks_count--;
                    bdirty(gb);
                    fs->sb->free_blocks_count--;
                    bdirty(fs->sb_buf);
                }
                brelse(bb);
            }
        }
        brelse(gb);
        if (bit >= 0)
            return fs->first_data + g * fs->bpg + bit;
    }
    return 0;
}

static void free_block(struct ext2 *fs, uint32_t blk)
{
    if (blk < fs->first_data || blk >= fs->blocks)
        return;                         /* nonsense: don't make it worse */
    uint32_t g = (blk - fs->first_data) / fs->bpg;
    struct ext2_gd *gd;
    struct buf *gb = get_group(fs, g, &gd);
    if (!gb)
        return;
    struct buf *bb = bread(fs->dev, gd->block_bitmap, fs->bsize);
    if (bb) {
        if (clear_bit(bb->data, (blk - fs->first_data) % fs->bpg)) {
            bdirty(bb);
            gd->free_blocks_count++;
            bdirty(gb);
            fs->sb->free_blocks_count++;
            bdirty(fs->sb_buf);
        }
        brelse(bb);
    }
    brelse(gb);
}

/* A free inode number, in or after `group` (0: none left). */
static uint32_t alloc_inode(struct ext2 *fs, uint32_t group, int is_dir)
{
    for (uint32_t t = 0; t < fs->groups; t++) {
        uint32_t g = (group + t) % fs->groups;
        struct ext2_gd *gd;
        struct buf *gb = get_group(fs, g, &gd);
        if (!gb)
            return 0;
        int bit = -1;
        if (gd->free_inodes_count) {
            struct buf *ib = bread(fs->dev, gd->inode_bitmap, fs->bsize);
            if (ib) {
                bit = take_bit(ib->data, fs->ipg);
                if (bit >= 0) {
                    bdirty(ib);
                    gd->free_inodes_count--;
                    if (is_dir)
                        gd->used_dirs_count++;
                    bdirty(gb);
                    fs->sb->free_inodes_count--;
                    bdirty(fs->sb_buf);
                }
                brelse(ib);
            }
        }
        brelse(gb);
        if (bit >= 0)
            return g * fs->ipg + bit + 1;
    }
    return 0;
}

static void free_inode(struct ext2 *fs, uint32_t ino, int is_dir)
{
    uint32_t g = group_of(fs, ino);
    struct ext2_gd *gd;
    struct buf *gb = get_group(fs, g, &gd);
    if (!gb)
        return;
    struct buf *ib = bread(fs->dev, gd->inode_bitmap, fs->bsize);
    if (ib) {
        if (clear_bit(ib->data, (ino - 1) % fs->ipg)) {
            bdirty(ib);
            gd->free_inodes_count++;
            if (is_dir && gd->used_dirs_count)
                gd->used_dirs_count--;
            bdirty(gb);
            fs->sb->free_inodes_count++;
            bdirty(fs->sb_buf);
        }
        brelse(ib);
    }
    brelse(gb);
}

/* --- where a file's blocks are -------------------------------------------- */

/*
 * The disk block holding block n of a file (0: a hole). With `alloc`,
 * missing blocks — and the indirect blocks on the way to them — are
 * allocated and zeroed; `dib` is the buffer holding the inode (dirty if
 * it changed). Cost: one cached block per indirection level.
 */
static int bmap(struct ext2 *fs, struct ext2_inode *di, struct buf *dib, uint32_t goal,
                uint32_t n, int alloc, uint32_t *out)
{
    uint32_t p = fs->ptrs, idx[3], *slot;
    int depth;
    if (n < 12) {
        depth = 0;
        slot = &di->block[n];
    } else if ((n -= 12) < p) {
        depth = 1;
        idx[0] = n;
        slot = &di->block[12];
    } else if ((n -= p) < p * p) {
        depth = 2;
        idx[0] = n / p;
        idx[1] = n % p;
        slot = &di->block[13];
    } else {
        n -= p * p;
        depth = 3;
        idx[0] = n / (p * p);
        idx[1] = n / p % p;
        idx[2] = n % p;
        slot = &di->block[14];
        if (idx[0] >= p)
            return -EFBIG;
    }

    struct buf *holder = NULL;          /* the indirect block `slot` is in */
    int err = 0;
    for (int level = 0; ; level++) {
        if (*slot == 0) {
            if (!alloc) {
                *out = 0;
                break;
            }
            uint32_t nb = alloc_block(fs, goal);
            if (!nb) {
                err = -ENOSPC;
                break;
            }
            struct buf *z = bclear(fs->dev, nb, fs->bsize);     /* zeros, no read */
            if (!z) {
                free_block(fs, nb);
                err = -EIO;
                break;
            }
            bdirty(z);
            brelse(z);
            *slot = nb;
            di->blocks += fs->bsize / 512;
            bdirty(holder ? holder : dib);
            if (holder)
                bdirty(dib);            /* i_blocks changed */
        }
        if (level == depth) {
            *out = *slot;
            break;
        }
        struct buf *b = bread(fs->dev, *slot, fs->bsize);
        if (!b) {
            err = -EIO;
            break;
        }
        if (holder)
            brelse(holder);
        holder = b;
        slot = (uint32_t *)b->data + idx[level];
    }
    if (holder)
        brelse(holder);
    return err;
}

/* Free the blocks under *slot — a tree `level` deep (0: *slot is a data
   block) — from relative block `from` on. The indirect block itself goes
   too when nothing before `from` is left in it. */
static void free_branch(struct ext2 *fs, struct ext2_inode *di, uint32_t *slot, int level,
                        uint32_t from)
{
    if (!*slot)
        return;
    if (level > 0) {
        uint32_t span = 1;
        for (int l = 1; l < level; l++)
            span *= fs->ptrs;
        struct buf *b = bread(fs->dev, *slot, fs->bsize);
        if (!b)
            return;
        uint32_t *p = (uint32_t *)b->data;
        for (uint32_t k = from / span; k < fs->ptrs; k++)
            free_branch(fs, di, &p[k], level - 1, k == from / span ? from % span : 0);
        bdirty(b);
        brelse(b);
    }
    if (from == 0) {
        free_block(fs, *slot);
        di->blocks -= fs->bsize / 512;
        *slot = 0;
    }
}

/* Free every block of the file from block `keep` on (the caller marks the
   inode's buffer dirty). Proportional to the file's size. */
static void free_from(struct ext2 *fs, struct ext2_inode *di, uint32_t keep)
{
    for (uint32_t k = keep; k < 12; k++)
        free_branch(fs, di, &di->block[k], 0, 0);
    uint32_t base = 12, span = fs->ptrs;
    for (int level = 1; level <= 3; level++) {
        uint32_t from = keep > base ? keep - base : 0;
        if (from < span)
            free_branch(fs, di, &di->block[11 + level], level, from);
        base += span;
        span *= fs->ptrs;
    }
}

/* Is this inode's block[] a list of blocks? Not for a small symlink (its
   target is in there) or a device (its number is). */
static int has_blocks(const struct ext2_inode *di)
{
    uint32_t t = di->mode & S_IFMT;
    if (t == S_IFLNK)
        return di->blocks != 0;
    return t == S_IFREG || t == S_IFDIR;
}

/* --- directories ------------------------------------------------------------- */

static uint8_t file_type(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFREG: return FT_REG;
    case S_IFDIR: return FT_DIR;
    case S_IFCHR: return FT_CHR;
    case S_IFLNK: return FT_LNK;
    case S_IFIFO: return FT_FIFO;
    default:      return FT_UNKNOWN;
    }
}

/* Find `name` in a directory. Returns the (held) buffer of the block it's
   in, the entry, and the entry before it in that block (or NULL). */
static struct buf *find_entry(struct ext2 *fs, struct ext2_inode *ddi, const char *name,
                              uint32_t len, struct ext2_dirent **out, struct ext2_dirent **prev)
{
    for (uint32_t blk = 0; blk < ddi->size / fs->bsize; blk++) {
        uint32_t disk;
        if (bmap(fs, ddi, NULL, 0, blk, 0, &disk) < 0 || !disk)
            continue;
        struct buf *b = bread(fs->dev, disk, fs->bsize);
        if (!b)
            return NULL;
        *prev = NULL;
        for (uint32_t off = 0; off + 8 <= fs->bsize; ) {
            struct ext2_dirent *e = (struct ext2_dirent *)(b->data + off);
            if (e->rec_len < 8 || off + e->rec_len > fs->bsize)
                break;                  /* damaged: skip the rest of the block */
            if (e->inode && e->name_len == len && memcmp(e->name, name, len) == 0) {
                *out = e;
                return b;
            }
            *prev = e;
            off += e->rec_len;
        }
        brelse(b);
    }
    return NULL;
}

/* We don't keep a directory's hash index (ext3/4 "dir_index") up to date,
   so a directory we change loses its flag; Linux then reads it the plain
   way, as we do. (Linux's own ext2 driver does the same.) */
static void touch_dir(struct ext2_inode *ddi, struct buf *dib, uint32_t now)
{
    ddi->mtime = ddi->ctime = now;
    ddi->flags &= ~INDEX_FL;
    bdirty(dib);
}

static int remove_entry(struct ext2 *fs, struct inode *dir, const char *name, uint32_t len)
{
    struct ext2_inode *ddi;
    struct buf *dib = get_inode(fs, dir->fs_ino, &ddi);
    if (!dib)
        return -EIO;
    struct ext2_dirent *e, *prev;
    struct buf *b = find_entry(fs, ddi, name, len, &e, &prev);
    if (!b) {
        brelse(dib);
        return -ENOENT;
    }
    if (prev)
        prev->rec_len += e->rec_len;    /* the one before swallows it */
    else
        e->inode = 0;                   /* first in its block: mark unused */
    bdirty(b);
    brelse(b);
    touch_dir(ddi, dib, time_now());
    brelse(dib);
    return 0;
}

static void fill_entry(struct ext2 *fs, struct ext2_dirent *e, const char *name,
                       uint32_t len, uint32_t ino, uint8_t type)
{
    e->inode = ino;
    e->name_len = len;
    e->file_type = fs->filetype ? type : 0;
    memcpy(e->name, name, len);
}

/* Add `name` -> ino to a directory: in the first gap big enough, or in a
   new block at its end. */
static int add_entry(struct ext2 *fs, struct inode *dir, const char *name, uint32_t len,
                     uint32_t ino, uint8_t type)
{
    struct ext2_inode *ddi;
    struct buf *dib = get_inode(fs, dir->fs_ino, &ddi);
    if (!dib)
        return -EIO;
    uint32_t need = DIRENT_LEN(len), nblocks = ddi->size / fs->bsize;
    int err = 0;
    for (uint32_t blk = 0; blk < nblocks; blk++) {
        uint32_t disk;
        if (bmap(fs, ddi, NULL, 0, blk, 0, &disk) < 0 || !disk)
            continue;
        struct buf *b = bread(fs->dev, disk, fs->bsize);
        if (!b) {
            brelse(dib);
            return -EIO;
        }
        for (uint32_t off = 0; off + 8 <= fs->bsize; ) {
            struct ext2_dirent *e = (struct ext2_dirent *)(b->data + off);
            if (e->rec_len < 8 || off + e->rec_len > fs->bsize)
                break;
            uint32_t used = e->inode ? DIRENT_LEN(e->name_len) : 0;
            if (e->rec_len >= used + need) {
                if (used) {             /* split: the new entry takes the slack */
                    struct ext2_dirent *n = (struct ext2_dirent *)((uint8_t *)e + used);
                    n->rec_len = e->rec_len - used;
                    e->rec_len = used;
                    e = n;
                }
                fill_entry(fs, e, name, len, ino, type);
                bdirty(b);
                brelse(b);
                goto done;
            }
            off += e->rec_len;
        }
        brelse(b);
    }
    /* No room: one more block. */
    uint32_t disk;
    err = bmap(fs, ddi, dib, group_of(fs, dir->fs_ino), nblocks, 1, &disk);
    if (!err) {
        struct buf *b = bread(fs->dev, disk, fs->bsize);
        if (!b) {
            err = -EIO;
        } else {
            struct ext2_dirent *e = (struct ext2_dirent *)b->data;
            e->rec_len = fs->bsize;
            fill_entry(fs, e, name, len, ino, type);
            bdirty(b);
            brelse(b);
            ddi->size += fs->bsize;
            dir->size = ddi->size;
        }
    }
done:
    if (!err)
        touch_dir(ddi, dib, time_now());
    brelse(dib);
    return err;
}

/* --- inodes <-> the tree ------------------------------------------------------ */

/* Our device numbers are (major << 8) | minor; ext2 stores small ones like
   that in block[0] ("old" encoding), bigger ones in block[1]. */
static uint32_t dev_from_disk(const struct ext2_inode *di)
{
    if (di->block[0])
        return di->block[0] & 0xFFFF;
    uint32_t b = di->block[1];
    return ((b >> 8) & 0xFFF) << 8 | (b & 0xFF);
}

static void copy_attrs(struct inode *i, const struct ext2_inode *di)
{
    i->mode = di->mode;
    i->size = di->size;
    i->nlink = di->links_count;
    i->uid = di->uid | (uint32_t)di->uid_high << 16;
    i->gid = di->gid | (uint32_t)di->gid_high << 16;
    i->atime = di->atime;
    i->mtime = di->mtime;
    i->ctime = di->ctime;
}

static const struct file_ops ext2_file_ops;

/* A symlink's target, into a copy the inode owns. */
static int load_symlink(struct ext2 *fs, struct inode *i, struct ext2_inode *di)
{
    uint32_t n = min(di->size, PATH_MAX);
    char *t = kmalloc(n ? n : 1);
    if (!t)
        return -ENOMEM;
    if (!has_blocks(di)) {
        memcpy(t, di->block, min(n, sizeof di->block));     /* a "fast" symlink */
    } else {
        uint32_t disk;
        struct buf *b = NULL;
        if (bmap(fs, di, NULL, 0, 0, 0, &disk) == 0 && disk)
            b = bread(fs->dev, disk, fs->bsize);
        if (!b) {
            kfree(t);
            return -EIO;
        }
        memcpy(t, b->data, min(n, fs->bsize));
        brelse(b);
    }
    i->data = (const uint8_t *)t;
    i->size = n;
    i->flags |= I_DATA_OWNED;
    return 0;
}

static int ext2_load_dir(struct inode *dir)
{
    struct ext2 *fs = fs_of(dir);
    uint32_t flags = lock(fs);
    if (!(dir->flags & I_UNLOADED)) {   /* done while we waited for the lock */
        unlock(fs, flags);
        return 0;
    }
    dir->flags &= ~I_UNLOADED;          /* (also on failure: no second try) */
    struct ext2_inode *ddi;
    struct buf *dib = get_inode(fs, dir->fs_ino, &ddi);
    int err = dib ? 0 : -EIO;
    for (uint32_t blk = 0; !err && blk < ddi->size / fs->bsize; blk++) {
        uint32_t disk;
        if (bmap(fs, ddi, NULL, 0, blk, 0, &disk) < 0 || !disk)
            continue;
        struct buf *b = bread(fs->dev, disk, fs->bsize);
        if (!b) {
            err = -EIO;
            break;
        }
        for (uint32_t off = 0; off + 8 <= fs->bsize; ) {
            struct ext2_dirent *e = (struct ext2_dirent *)(b->data + off);
            if (e->rec_len < 8 || off + e->rec_len > fs->bsize)
                break;
            off += e->rec_len;
            if (!e->inode || vfs_is_dots(e->name, e->name_len))
                continue;
            struct ext2_inode *cdi;
            struct buf *cb = get_inode(fs, e->inode, &cdi);
            char *name = kmalloc(e->name_len ? e->name_len : 1);
            struct inode *c = NULL;
            if (cb && name) {
                memcpy(name, e->name, e->name_len);
                c = vfs_add(dir, name, e->name_len, cdi->mode);
            }
            if (!c) {
                kfree(name);
                if (cb)
                    brelse(cb);
                err = cb ? -ENOMEM : -EIO;
                break;
            }
            c->flags |= I_NAME_OWNED;
            c->fs = dir->fs;
            c->mnt = dir->mnt;
            c->fs_ino = e->inode;
            copy_attrs(c, cdi);
            if (S_ISDIR(c->mode))
                c->flags |= I_UNLOADED;         /* read when someone looks */
            else if (S_ISREG(c->mode))
                c->ops = &ext2_file_ops;
            else if (S_ISLNK(c->mode))
                load_symlink(fs, c, cdi);
            else if (S_ISCHR(c->mode)) {
                c->rdev = dev_from_disk(cdi);
                dev_attach(c);
            }
            brelse(cb);
        }
        brelse(b);
    }
    if (dib) {
        dir->nlink = ddi->links_count;  /* (vfs_add counted subdirectories again) */
        brelse(dib);
    }
    unlock(fs, flags);
    return err;
}

static int ext2_create(struct inode *dir, struct inode *i)
{
    struct ext2 *fs = fs_of(dir);
    if (fs->readonly)
        return -EROFS;
    uint32_t flags = lock(fs);
    int is_dir = S_ISDIR(i->mode), err = 0;
    uint32_t ino = alloc_inode(fs, group_of(fs, dir->fs_ino), is_dir);
    if (!ino) {
        unlock(fs, flags);
        return -ENOSPC;
    }
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, ino, &di);
    if (!ib) {
        free_inode(fs, ino, is_dir);
        unlock(fs, flags);
        return -EIO;
    }
    memset(di, 0, fs->isize);
    di->mode = i->mode;
    di->uid = i->uid & 0xFFFF;
    di->uid_high = i->uid >> 16;
    di->gid = i->gid & 0xFFFF;
    di->gid_high = i->gid >> 16;
    di->atime = i->atime;
    di->mtime = i->mtime;
    di->ctime = i->ctime;
    di->links_count = is_dir ? 2 : 1;   /* a directory's "." counts too */
    bdirty(ib);

    if (is_dir) {                       /* its first block: "." and ".." */
        uint32_t disk;
        err = bmap(fs, di, ib, group_of(fs, ino), 0, 1, &disk);
        struct buf *b = err ? NULL : bread(fs->dev, disk, fs->bsize);
        if (b) {
            struct ext2_dirent *dot = (struct ext2_dirent *)b->data;
            fill_entry(fs, dot, ".", 1, ino, FT_DIR);
            dot->rec_len = DIRENT_LEN(1);
            struct ext2_dirent *dotdot = (struct ext2_dirent *)(b->data + dot->rec_len);
            fill_entry(fs, dotdot, "..", 2, dir->fs_ino, FT_DIR);
            dotdot->rec_len = fs->bsize - dot->rec_len;
            bdirty(b);
            brelse(b);
            di->size = fs->bsize;
        } else if (!err) {
            err = -EIO;
        }
    } else if (S_ISLNK(i->mode)) {
        di->size = i->size;
        if (i->size < sizeof di->block) {
            memcpy(di->block, i->data, i->size);    /* small: in the inode */
        } else {
            uint32_t disk;
            err = bmap(fs, di, ib, group_of(fs, ino), 0, 1, &disk);
            struct buf *b = err ? NULL : bread(fs->dev, disk, fs->bsize);
            if (b) {
                memcpy(b->data, i->data, min(i->size, fs->bsize));
                bdirty(b);
                brelse(b);
            } else if (!err) {
                err = -EIO;
            }
        }
    } else if (S_ISCHR(i->mode)) {
        di->block[0] = i->rdev;
    }
    if (!err)
        err = add_entry(fs, dir, i->name, i->name_len, ino, file_type(i->mode));
    if (err) {                          /* undo: the inode and its blocks */
        free_from(fs, di, 0);
        di->links_count = 0;
        di->dtime = time_now();
        free_inode(fs, ino, is_dir);
    } else {
        i->fs_ino = ino;
        i->nlink = di->links_count;
        i->size = S_ISLNK(i->mode) ? i->size : di->size;
        if (S_ISREG(i->mode))
            i->ops = &ext2_file_ops;
        if (is_dir) {                   /* the parent gains a link: our ".." */
            struct ext2_inode *pdi;
            struct buf *pb = get_inode(fs, dir->fs_ino, &pdi);
            if (pb) {
                pdi->links_count++;
                bdirty(pb);
                brelse(pb);
            }
        }
    }
    bdirty(ib);
    brelse(ib);
    unlock(fs, flags);
    return err;
}

/* One fewer name for the disk inode (and for a directory, both its "." and
   its parent's link from our ".."). At 0 it's freed in evict, once nobody
   uses it any more. */
static void drop_link(struct ext2 *fs, struct inode *i, struct inode *parent)
{
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, i->fs_ino, &di);
    if (!ib)
        return;
    if (S_ISDIR(i->mode)) {
        di->links_count = 0;
        struct ext2_inode *pdi;
        struct buf *pb = get_inode(fs, parent->fs_ino, &pdi);
        if (pb) {
            if (pdi->links_count > 2)
                pdi->links_count--;
            bdirty(pb);
            brelse(pb);
        }
    } else if (di->links_count) {
        di->links_count--;
    }
    di->ctime = time_now();
    bdirty(ib);
    brelse(ib);
}

static int ext2_unlink(struct inode *dir, struct inode *i)
{
    struct ext2 *fs = fs_of(dir);
    if (fs->readonly)
        return -EROFS;
    uint32_t flags = lock(fs);
    int err = remove_entry(fs, dir, i->name, i->name_len);
    if (!err)
        drop_link(fs, i, dir);
    unlock(fs, flags);
    return err;
}

static int ext2_rename(struct inode *i, struct inode *new_dir, const char *name,
                       uint32_t len, struct inode *replaced)
{
    struct ext2 *fs = fs_of(i);
    if (fs->readonly)
        return -EROFS;
    uint32_t flags = lock(fs);
    struct inode *old_dir = i->parent;
    int err = 0;
    if (replaced) {                     /* point the existing name at i */
        struct ext2_inode *ddi;
        struct buf *dib = get_inode(fs, new_dir->fs_ino, &ddi);
        struct ext2_dirent *e, *prev;
        struct buf *b = dib ? find_entry(fs, ddi, name, len, &e, &prev) : NULL;
        if (b) {
            e->inode = i->fs_ino;
            e->file_type = fs->filetype ? file_type(i->mode) : 0;
            bdirty(b);
            brelse(b);
            touch_dir(ddi, dib, time_now());
            drop_link(fs, replaced, new_dir);
        } else {
            err = dib ? -ENOENT : -EIO;
        }
        if (dib)
            brelse(dib);
    } else {
        err = add_entry(fs, new_dir, name, len, i->fs_ino, file_type(i->mode));
    }
    if (!err)                           /* then the old name goes */
        err = remove_entry(fs, old_dir, i->name, i->name_len);

    if (!err && S_ISDIR(i->mode) && old_dir != new_dir) {
        /* Its ".." now means new_dir: the second entry of its first block. */
        struct ext2_inode *di;
        struct buf *ib = get_inode(fs, i->fs_ino, &di);
        uint32_t disk;
        if (ib && bmap(fs, di, NULL, 0, 0, 0, &disk) == 0 && disk) {
            struct buf *b = bread(fs->dev, disk, fs->bsize);
            if (b) {
                struct ext2_dirent *dot = (struct ext2_dirent *)b->data;
                struct ext2_dirent *dotdot = (struct ext2_dirent *)(b->data + dot->rec_len);
                dotdot->inode = new_dir->fs_ino;
                bdirty(b);
                brelse(b);
            }
        }
        if (ib)
            brelse(ib);
        struct ext2_inode *pdi;
        struct buf *pb = get_inode(fs, old_dir->fs_ino, &pdi);
        if (pb) {
            pdi->links_count--;
            bdirty(pb);
            brelse(pb);
        }
        pb = get_inode(fs, new_dir->fs_ino, &pdi);
        if (pb) {
            pdi->links_count++;
            bdirty(pb);
            brelse(pb);
        }
    }
    unlock(fs, flags);
    return err;
}

static void ext2_evict(struct inode *i)
{
    struct ext2 *fs = fs_of(i);
    if (fs->readonly)
        return;
    uint32_t flags = lock(fs);
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, i->fs_ino, &di);
    if (ib && di->links_count == 0) {   /* no name left anywhere: free it */
        int is_dir = S_ISDIR(di->mode);
        if (has_blocks(di))
            free_from(fs, di, 0);
        di->dtime = time_now();
        bdirty(ib);
        free_inode(fs, i->fs_ino, is_dir);
    }
    if (ib)
        brelse(ib);
    unlock(fs, flags);
}

static int ext2_setattr(struct inode *i)
{
    struct ext2 *fs = fs_of(i);
    if (fs->readonly)
        return -EROFS;
    uint32_t flags = lock(fs);
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, i->fs_ino, &di);
    if (ib) {
        di->mode = i->mode;
        di->uid = i->uid & 0xFFFF;
        di->uid_high = i->uid >> 16;
        di->gid = i->gid & 0xFFFF;
        di->gid_high = i->gid >> 16;
        di->atime = i->atime;
        di->mtime = i->mtime;
        di->ctime = i->ctime;
        bdirty(ib);
        brelse(ib);
    }
    unlock(fs, flags);
    return ib ? 0 : -EIO;
}

static int ext2_truncate(struct inode *i, uint32_t size)
{
    struct ext2 *fs = fs_of(i);
    if (fs->readonly)
        return -EROFS;
    uint32_t flags = lock(fs);
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, i->fs_ino, &di);
    if (!ib) {
        unlock(fs, flags);
        return -EIO;
    }
    if (size < di->size) {
        free_from(fs, di, size / fs->bsize + (size % fs->bsize != 0));
        /* Zero the cut-off end of the last block: growing the file again
           must show zeros there, not the old bytes. */
        uint32_t disk;
        if (size % fs->bsize &&
            bmap(fs, di, NULL, 0, size / fs->bsize, 0, &disk) == 0 && disk) {
            struct buf *b = bread(fs->dev, disk, fs->bsize);
            if (b) {
                memset(b->data + size % fs->bsize, 0, fs->bsize - size % fs->bsize);
                bdirty(b);
                brelse(b);
            }
        }
    }
    di->size = i->size = size;          /* growing just leaves a hole */
    di->mtime = di->ctime = i->mtime = i->ctime = time_now();
    bdirty(ib);
    brelse(ib);
    unlock(fs, flags);
    return 0;
}

static int ext2_read_kernel(struct inode *i, uint32_t pos, void *kbuf, uint32_t n)
{
    struct ext2 *fs = fs_of(i);
    uint32_t flags = lock(fs);
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, i->fs_ino, &di);
    int err = ib ? 0 : -EIO;
    uint8_t *dst = kbuf;
    while (n) {
        uint32_t in = pos % fs->bsize, k = min(fs->bsize - in, n), disk = 0;
        struct buf *b = NULL;
        if (!err && bmap(fs, di, NULL, 0, pos / fs->bsize, 0, &disk) == 0 && disk)
            b = bread(fs->dev, disk, fs->bsize);
        if (b) {
            memcpy(dst, b->data + in, k);
            brelse(b);
        } else {
            memset(dst, 0, k);          /* a hole (or a read error) */
            if (disk)
                err = -EIO;
        }
        dst += k;
        pos += k;
        n -= k;
    }
    if (ib)
        brelse(ib);
    unlock(fs, flags);
    return err;
}

static int ext2_statfs(struct mount *m, uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                       uint32_t *files, uint32_t *ffree)
{
    struct ext2 *fs = (struct ext2 *)m;
    *bsize = fs->bsize;
    *blocks = fs->sb->blocks_count;
    *bfree = fs->sb->free_blocks_count;
    *files = fs->sb->inodes_count;
    *ffree = fs->sb->free_inodes_count;
    return 0;
}

/* --- read() and write() --------------------------------------------------------- */

/* Touch every page of a user buffer before taking the lock: a page of an
   mmap'd disk file that isn't in memory yet is read by the page fault —
   which needs this same lock. */
static int prefault(const void *user_buf, uint32_t n)
{
    uint8_t c;
    for (uintptr_t p = (uintptr_t)user_buf, end = p + n; p < end; p = (p & ~4095u) + 4096)
        if (copy_from_user(&c, (const void *)p, 1) < 0)
            return -EFAULT;
    return 0;
}

static int32_t ext2_read(struct file *f, void *user_buf, uint32_t n)
{
    static const uint8_t zeros[4096];
    struct inode *i = f->inode;
    struct ext2 *fs = fs_of(i);
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (f->pos >= i->size)
        return 0;                       /* end of file */
    n = min(min(n, i->size - f->pos), RW_MAX);
    if (prefault(user_buf, n) < 0)
        return -EFAULT;
    uint32_t flags = lock(fs);
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, i->fs_ino, &di);
    int err = ib ? 0 : -EIO;
    uint32_t done = 0;
    while (!err && done < n) {
        uint32_t pos = f->pos + done, in = pos % fs->bsize, k = min(fs->bsize - in, n - done);
        uint32_t disk;
        err = bmap(fs, di, NULL, 0, pos / fs->bsize, 0, &disk);
        if (err)
            break;
        struct buf *b = disk ? bread(fs->dev, disk, fs->bsize) : NULL;
        if (disk && !b) {
            err = -EIO;
            break;
        }
        if (copy_to_user((uint8_t *)user_buf + done, b ? b->data + in : zeros, k) < 0)
            err = -EFAULT;
        if (b)
            brelse(b);
        if (!err)
            done += k;
    }
    if (ib)
        brelse(ib);
    unlock(fs, flags);
    if (!done && err)
        return err;
    f->pos += done;
    return done;
}

static int32_t ext2_write(struct file *f, const void *user_buf, uint32_t n)
{
    struct inode *i = f->inode;
    struct ext2 *fs = fs_of(i);
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (fs->readonly)
        return -EROFS;
    n = min(n, RW_MAX);
    if (n == 0)
        return 0;
    if (prefault(user_buf, n) < 0)
        return -EFAULT;
    uint32_t flags = lock(fs);
    if (f->flags & O_APPEND)
        f->pos = i->size;               /* every write goes to the end */
    uint32_t pos = f->pos;
    if (pos + n < pos) {
        unlock(fs, flags);
        return -EFBIG;                  /* past 4 GiB */
    }
    struct ext2_inode *di;
    struct buf *ib = get_inode(fs, i->fs_ino, &di);
    int err = ib ? 0 : -EIO;
    uint32_t done = 0, goal = group_of(fs, i->fs_ino);
    while (!err && done < n) {
        uint32_t p = pos + done, in = p % fs->bsize, k = min(fs->bsize - in, n - done);
        uint32_t disk;
        err = bmap(fs, di, ib, goal, p / fs->bsize, 1, &disk);
        if (err)
            break;
        /* A whole block replaced needn't be read first. */
        struct buf *b = (in == 0 && k == fs->bsize) ? bclear(fs->dev, disk, fs->bsize)
                                                    : bread(fs->dev, disk, fs->bsize);
        if (!b) {
            err = -EIO;
            break;
        }
        if (copy_from_user(b->data + in, (const uint8_t *)user_buf + done, k) < 0)
            err = -EFAULT;
        bdirty(b);
        brelse(b);
        if (!err)
            done += k;
    }
    if (ib) {
        if (pos + done > di->size)
            di->size = i->size = pos + done;
        di->mtime = di->ctime = i->mtime = i->ctime = time_now();
        bdirty(ib);
        brelse(ib);
    }
    unlock(fs, flags);
    if (!done)
        return err;
    f->pos += done;
    return done;
}

static const struct file_ops ext2_file_ops = { .read = ext2_read, .write = ext2_write };

static const struct fs_ops ext2_ops = {
    .load_dir = ext2_load_dir,
    .create = ext2_create,
    .unlink = ext2_unlink,
    .rename = ext2_rename,
    .evict = ext2_evict,
    .setattr = ext2_setattr,
    .truncate = ext2_truncate,
    .read_kernel = ext2_read_kernel,
    .statfs = ext2_statfs,
};

/* --- mounting --------------------------------------------------------------------- */

int ext2_mount(struct blockdev *dev, const char *path)
{
    /* The superblock, read once directly (before we know the block size). */
    static struct { struct ext2_super s; uint8_t rest[1024 - sizeof(struct ext2_super)]; } raw;
    if (blockdev_read(dev, 2, 2, &raw) < 0 || raw.s.magic != EXT2_MAGIC)
        return -EINVAL;                 /* not ext2 */
    struct ext2_super *s = &raw.s;
    uint32_t incompat = s->rev_level ? s->feature_incompat : 0;
    uint32_t ro_compat = s->rev_level ? s->feature_ro_compat : 0;
    if (s->log_block_size > 2 || (incompat & ~INCOMPAT_FILETYPE)) {
        kprintf("ext2: %s uses features we can't read (incompat %lx, %lu-byte blocks)\n",
                dev->name, incompat, (uint32_t)1024 << s->log_block_size);
        return -EINVAL;
    }
    struct ext2 *fs = kmalloc(sizeof *fs);
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof *fs);
    fs->dev = dev;
    fs->bsize = 1024u << s->log_block_size;
    fs->ptrs = fs->bsize / 4;
    fs->ipg = s->inodes_per_group;
    fs->bpg = s->blocks_per_group;
    fs->first_data = s->first_data_block;
    fs->blocks = s->blocks_count;
    fs->inodes = s->inodes_count;
    fs->isize = s->rev_level ? s->inode_size : 128;
    fs->first_ino = s->rev_level ? s->first_ino : 11;
    fs->filetype = (incompat & INCOMPAT_FILETYPE) != 0;
    fs->readonly = (ro_compat & ~(RO_COMPAT_SPARSE | RO_COMPAT_LARGEFILE)) != 0;
    fs->groups = (fs->blocks - fs->first_data + fs->bpg - 1) / fs->bpg;
    fs->gd_block = fs->first_data + 1;
    if (fs->isize < 128 || fs->isize > fs->bsize || !fs->ipg || !fs->bpg) {
        kfree(fs);
        return -EINVAL;
    }

    /* From now on the superblock lives in the cache, as part of the block
       it's in (block 1 with 1 KiB blocks, else byte 1024 of block 0). */
    uint32_t sb_block = fs->bsize == 1024 ? 1 : 0, sb_off = fs->bsize == 1024 ? 0 : 1024;
    fs->sb_buf = bread(dev, sb_block, fs->bsize);
    if (!fs->sb_buf) {
        kfree(fs);
        return -EIO;
    }
    fs->sb = (struct ext2_super *)(fs->sb_buf->data + sb_off);
    if (!fs->readonly) {                /* "in use": e2fsck checks it if we crash */
        fs->sb->state &= ~VALID_FS;
        fs->sb->mnt_count++;
        fs->sb->mtime = time_now();
        bdirty(fs->sb_buf);
    }

    fs->m.ops = &ext2_ops;
    fs->m.priv = fs;
    fs->m.type = "ext2";
    struct ext2_inode *rdi;
    struct buf *rb = get_inode(fs, ROOT_INO, &rdi);
    int err = rb ? vfs_mount(path, &fs->m, ROOT_INO, rdi->mode) : -EIO;
    if (err) {
        if (rb)
            brelse(rb);
        brelse(fs->sb_buf);
        kfree(fs);
        return err;
    }
    copy_attrs(fs->m.root, rdi);
    brelse(rb);
    kprintf("ext2: %s mounted at %s: %lu MiB, %lu-byte blocks, %lu%% free%s\n",
            dev->name, path, (uint32_t)((uint64_t)fs->blocks * fs->bsize >> 20), fs->bsize,
            (uint32_t)((uint64_t)fs->sb->free_blocks_count * 100 / fs->blocks),
            fs->readonly ? " (read-only: unknown features)" : "");
    return 0;
}
