/*
 * pcache.c — the page cache: file data, one 4 KiB frame per (file, page)
 *
 * Every read of a disk file — read(), exec, an mmap page fault — comes
 * through here, so a page that's in RAM costs one lookup and one copy.
 * (The block cache underneath keeps only the file system's own blocks:
 * inodes, bitmaps, directories, indirect blocks.)
 *
 * Speed and predictability (CLAUDE.md rules 1, 5, 6):
 *   - 4-way set-associative: a page can only live in one of 4 slots, picked
 *     by a hash of (file, page number), so a lookup looks at 4 entries at
 *     most — never a search. A "clock" bit per slot picks which of the 4 to
 *     throw out: a page used since the last pass gets a second chance.
 *   - size fixed at boot: a quarter of RAM, at most 64 MiB; frames are
 *     taken on first use
 *   - a miss reads ahead: the file system maps a whole run of pages and the
 *     disk fills them straight from DMA (blockdev_readv), one command for up
 *     to 128 KiB — no copy, no 1 KiB-at-a-time commands. The run grows
 *     while a file is read in order (4, 8, 16, 32 pages) and is just what
 *     was asked for when it isn't.
 *   - forgetting a file's pages (a write) is O(1): the file gets a new id,
 *     and pages carrying the old one simply never match again; their slots
 *     are reused like any other.
 *
 * No locks: one CPU, and everything between two sleeps runs with
 * interrupts off. A reader pins a page while it copies (the copy to a user
 * buffer can fault and sleep), so it is never thrown out under the copy.
 */
#include <blockdev.h>
#include <errno.h>
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pcache.h>
#include <pmm.h>
#include <string.h>
#include <uaccess.h>
#include <vfs.h>

#define WAYS      4
#define SETS_MAX  4096                  /* 16384 pages: 64 MiB */
#define RA_START  4                     /* first read-ahead of a file, pages */
#define RA_MAX    (BIO_SECTORS * SECTOR_SIZE / PAGE_SIZE)   /* 32: 128 KiB */

struct pc_page {
    uint32_t id;                        /* the file's cache id (0: empty slot) */
    uint32_t index;                     /* page number in the file */
    uint32_t frame;                     /* physical; 0: no frame yet */
    uint16_t readers;                   /* copies in progress: don't evict */
    uint8_t  used;                      /* clock bit */
};

static struct pc_page *table;
static uint32_t set_mask;
static uint32_t next_id = 1;
static uint32_t hits, misses, nframes;

void pcache_init(void)
{
    uint32_t sets = 16;
    while (sets * 2 * WAYS <= pmm_usable_count() / 4 && sets < SETS_MAX)
        sets *= 2;
    table = kmalloc(sets * WAYS * sizeof *table);
    if (!table) {
        kprintf("pcache: no memory for the table\n");
        return;
    }
    memset(table, 0, sets * WAYS * sizeof *table);
    set_mask = sets - 1;
    kprintf("pcache: %lu KiB at most\n", sets * WAYS * PAGE_SIZE / 1024);
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static uint32_t id_of(struct inode *ino)
{
    if (!ino->pc_id) {
        if (next_id == 0) {             /* wrapped (after 2^32 writes): forget all */
            for (uint32_t k = 0; k <= set_mask * WAYS + WAYS - 1; k++)
                if (!table[k].readers)
                    table[k].id = 0;
            next_id = 1;
        }
        ino->pc_id = next_id++;
    }
    return ino->pc_id;
}

/* The pages of one file land in consecutive sets, so a file never collides
   with itself until it is bigger than the whole cache. */
static struct pc_page *set_of(uint32_t id, uint32_t index)
{
    return &table[((id * 0x9E3779B9u + index) & set_mask) * WAYS];
}

static struct pc_page *find(uint32_t id, uint32_t index)
{
    struct pc_page *s = set_of(id, index);
    for (int w = 0; w < WAYS; w++)
        if (s[w].id == id && s[w].index == index)
            return &s[w];
    return NULL;
}

/* The slot a new page goes in: an empty one, else the first unpinned one
   not used since the last pass (clearing the bits it passes). NULL if all
   four are pinned. */
static struct pc_page *victim(struct pc_page *s)
{
    for (int w = 0; w < WAYS; w++)
        if (!s[w].id)
            return &s[w];
    for (int pass = 0; pass < 2; pass++)
        for (int w = 0; w < WAYS; w++) {
            if (s[w].readers)
                continue;
            if (!s[w].used)
                return &s[w];
            s[w].used = 0;
        }
    return NULL;
}

/* Read pages [index, index + n) of the file into the cache (n >= 1, all
   missing). Sleeps for the disk. 0, or -EIO / -ENOMEM. */
static int fill(struct inode *ino, uint32_t index, uint32_t n)
{
    uint32_t frames[RA_MAX];
    uint32_t got = 0;
    while (got < n && (frames[got] = pmm_alloc()) != 0)
        got++;
    if (!got)
        return -ENOMEM;
    misses += got;

    uint32_t id = id_of(ino);
    int err = 0;
    if (ino->fs->readpages) {
        err = ino->fs->readpages(ino, index, got, frames);
    } else {
        for (uint32_t k = 0; k < got && !err; k++) {
            uint32_t off = (index + k) * PAGE_SIZE;
            uint32_t len = off < ino->size ? min_u32(ino->size - off, PAGE_SIZE) : 0;
            memset(P2V(frames[k]), 0, PAGE_SIZE);
            if (len)
                err = ino->fs->read_kernel(ino, off, P2V(frames[k]), len);
        }
    }

    /* Publish, unless a write changed the file while the disk was busy
       (its id moved on): then these bytes may be old. */
    uint32_t k = 0;
    if (!err && ino->pc_id == id) {
        for (; k < got; k++) {
            if (find(id, index + k))
                continue;               /* someone else read it meanwhile */
            struct pc_page *p = victim(set_of(id, index + k));
            if (!p)
                continue;
            if (p->frame)
                pmm_free(p->frame);
            else
                nframes++;
            p->id = id;
            p->index = index + k;
            p->frame = frames[k];
            p->used = 0;
            frames[k] = 0;
        }
    }
    for (k = 0; k < got; k++)
        if (frames[k])
            pmm_free(frames[k]);
    return err < 0 ? -EIO : 0;
}

/* How many pages to read for a miss on `index` when the caller wants
   `want` pages: more if the file is being read in order. Stops at the end
   of the file and at the first page that's already cached. */
static uint32_t window(struct inode *ino, uint32_t id, uint32_t index, uint32_t want)
{
    uint32_t n = want;
    if (index == ino->ra_next) {        /* in order: grow */
        uint32_t ra = ino->ra_win ? ino->ra_win * 2 : RA_START;
        if (ra > n)
            n = ra;
    }
    if (n > RA_MAX)
        n = RA_MAX;
    uint32_t last = ino->size ? (ino->size - 1) / PAGE_SIZE : 0;
    if (index > last)
        n = 1;
    else if (n > last - index + 1)
        n = last - index + 1;
    for (uint32_t k = 1; k < n; k++)
        if (find(id, index + k)) {
            n = k;
            break;
        }
    ino->ra_win = (uint16_t)n;
    ino->ra_next = index + n;           /* a miss there means "in order" */
    return n;
}

static int copy_out(struct inode *ino, uint32_t pos, void *dst, uint32_t n, int user)
{
    if (!ino->fs || !table)
        return -EIO;
    uint8_t *out = dst;
    uint32_t flags = irq_save();
    int err = 0, tries = 0;
    while (n) {
        uint32_t index = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        uint32_t k = min_u32(PAGE_SIZE - in, n);
        uint32_t id = id_of(ino);
        struct pc_page *p = find(id, index);
        if (!p) {
            if (++tries > 8) {          /* writes keep racing us, or no memory */
                err = -EIO;
                break;
            }
            uint32_t want = (pos + n - 1) / PAGE_SIZE - index + 1;
            err = fill(ino, index, window(ino, id, index, want));
            if (err)
                break;
            continue;                   /* look again: it may not have stuck */
        }
        hits++;
        tries = 0;
        p->used = 1;
        p->readers++;
        const uint8_t *src = (const uint8_t *)P2V(p->frame) + in;
        if (user)
            err = copy_to_user(out, src, k) < 0 ? -EFAULT : 0;
        else
            memcpy(out, src, k);
        p->readers--;
        if (err)
            break;
        out += k;
        pos += k;
        n -= k;
    }
    irq_restore(flags);
    int done = out - (uint8_t *)dst;
    return done ? done : err;           /* like read(): a short count first */
}

int pcache_read(struct inode *ino, uint32_t pos, void *dst, uint32_t n)
{
    int r = copy_out(ino, pos, dst, n, 0);
    return r == (int)n ? 0 : r < 0 ? r : -EIO;
}

int pcache_read_user(struct inode *ino, uint32_t pos, void *user_dst, uint32_t n)
{
    return copy_out(ino, pos, user_dst, n, 1);
}

void pcache_invalidate(struct inode *ino)
{
    uint32_t flags = irq_save();
    ino->pc_id = 0;                     /* a new id on next use: old pages never match */
    irq_restore(flags);
}

void pcache_stats(uint32_t *h, uint32_t *m)
{
    if (h)
        *h = hits;
    if (m)
        *m = misses;
}

uint32_t pcache_pages(void)
{
    return nframes;
}
