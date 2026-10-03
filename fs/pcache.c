/*
 * pcache.c — the page cache: file data, one 4 KiB frame per (file, page)
 *
 * Every read and write of a disk file — read(), write(), exec, an mmap page
 * fault — comes through here, so a page that's in RAM costs one lookup and
 * one copy. The block cache underneath keeps only the file system's own
 * blocks: inodes, bitmaps, directories, indirect blocks.
 *
 * Speed and predictability (CLAUDE.md rules 1, 5, 6):
 *   - 4-way set-associative: a page can only live in one of 4 slots, picked
 *     by (file, page number), so a lookup looks at 4 entries at most — never
 *     a search. A "clock" bit per slot picks which of the 4 to throw out.
 *   - it may use ALL free RAM (the table has a slot for every frame): idle
 *     RAM is wasted RAM, and a hit is ~10 000x cheaper than a disk seek (see
 *     docs/performance.md, "Cache sizing"). When the page allocator runs
 *     out it calls pcache_reclaim, which gives back clean pages.
 *   - a miss reads ahead: the file system maps a run of pages and the disk
 *     fills them straight from DMA, one command for up to 128 KiB.
 *   - writes land in the page and mark it dirty; write-back sends runs of
 *     dirty pages as one command each. Whole 4 KiB pages: every driver can
 *     take them as they are (no bounce copies).
 *   - forgetting a file's pages is O(its dirty pages): the file gets a new
 *     id, and pages carrying the old one simply never match again.
 *
 * No locks: one CPU, and everything between two sleeps runs with
 * interrupts off. A user of a page pins it (readers) while it copies or the
 * disk works on it, so it is never thrown out underneath.
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
#include <thread.h>
#include <uaccess.h>
#include <vfs.h>

#define WAYS       4
#define TABLE_MAX  (1u << 18)           /* slots: 1 GiB of cache */
#define RA_START   4                    /* first read-ahead of a file, pages */
#define RA_MAX     (BIO_SECTORS * SECTOR_SIZE / PAGE_SIZE)   /* 32: 128 KiB */
#define SCAN_MAX   1024                 /* pages one reclaim round looks at */
#define DIRTY_CAP  8192                 /* pages: 32 MiB */
#define RA_REQS    8                    /* read-ahead requests on their way at once */

#define P_USED   1                      /* clock bit */
#define P_DIRTY  2                      /* changed since it was last written */
#define P_IO     4                      /* the disk is still filling it (pinned) */
#define P_RA     8                      /* reaching it starts the next read-ahead */

struct pc_page {
    uint32_t id;                        /* the file's cache id (0: empty slot) */
    uint32_t index;                     /* page number in the file */
    uint32_t frame;                     /* physical; 0: no frame */
    uint16_t readers;                   /* pins: copies, disk I/O */
    uint8_t  flags;
};
_Static_assert(sizeof(struct pc_page) == 16, "one set (4 slots) per 64-byte cache line");

static struct pc_page *table;
static struct inode **owner;            /* per slot, only for dirty pages (for write-back) */
static uint32_t nslots, set_mask, hand;
static uint32_t next_id = 1;
static uint32_t hits, misses, ra_pages, nframes, ndirty, dirty_max;
static struct inode *dirty_list;        /* files with dirty pages */

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

uint32_t pcache_reclaim(uint32_t want);

void pcache_init(void)
{
    uint32_t frames = pmm_usable_count();
    nslots = 64;
    while (nslots < frames && nslots < TABLE_MAX)
        nslots *= 2;
    table = kmalloc(nslots * sizeof *table);
    owner = kmalloc(nslots * sizeof *owner);
    if (!table || !owner) {
        kprintf("pcache: no memory for the table\n");
        table = NULL;
        return;
    }
    memset(table, 0, nslots * sizeof *table);
    set_mask = nslots / WAYS - 1;
    /* RAM/8, at most 32 MiB: an old disk writes ~50 MB/s, so a sync never
       has more than ~0.6 s of work waiting (docs/performance.md). */
    dirty_max = frames / 8 > 64 ? frames / 8 : 64;
    if (dirty_max > DIRTY_CAP)
        dirty_max = DIRTY_CAP;
    pmm_set_reclaim(pcache_reclaim);
    kprintf("pcache: %lu slots (%lu KiB table), up to all free RAM\n",
            nslots, nslots * (sizeof *table + sizeof *owner) / 1024);
}

static uint32_t id_of(struct inode *ino)
{
    if (!ino->pc_id) {
        if (next_id == 0) {             /* wrapped (after 2^32 ids): forget clean pages */
            for (uint32_t k = 0; k < nslots; k++)
                if (!table[k].readers && !(table[k].flags & P_DIRTY))
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
        if (s[w].id == id && s[w].index == index && s[w].frame)
            return &s[w];
    return NULL;
}

static void drop(struct pc_page *p)
{
    if (p->frame) {
        pmm_free(p->frame);
        nframes--;
    }
    p->frame = 0;
    p->id = 0;
    p->flags = 0;
}

/* --- dirty pages ----------------------------------------------------------- */

static void mark_dirty(struct inode *ino, struct pc_page *p)
{
    if (p->flags & P_DIRTY)
        return;
    p->flags |= P_DIRTY;
    owner[p - table] = ino;
    ndirty++;
    if (!ino->pc_ndirty++) {
        ino->dirty_lo = ino->dirty_hi = p->index;
        ino->dirty_next = dirty_list;
        dirty_list = ino;
    }
    if (p->index < ino->dirty_lo)
        ino->dirty_lo = p->index;
    if (p->index > ino->dirty_hi)
        ino->dirty_hi = p->index;
}

static void unlist(struct inode *ino)
{
    for (struct inode **pp = &dirty_list; *pp; pp = &(*pp)->dirty_next)
        if (*pp == ino) {
            *pp = ino->dirty_next;
            break;
        }
}

static void clear_dirty(struct inode *ino, struct pc_page *p)
{
    p->flags &= ~P_DIRTY;
    ndirty--;
    if (!--ino->pc_ndirty)
        unlist(ino);
}

/* Write the file's dirty pages: runs of neighbours as one command each.
   Sleeps. The cost is the file's dirty range, not the cache. */
int pcache_flush(struct inode *ino)
{
    if (!table || !ino->pc_ndirty || !ino->fs || !ino->fs->writepages)
        return 0;
    uint32_t flags = irq_save();
    vfs_get(ino);                       /* it may be deleted while the disk works */
    int err = 0;
    uint32_t id = ino->pc_id;
    for (uint32_t idx = ino->dirty_lo; ino->pc_ndirty && idx <= ino->dirty_hi; ) {
        struct pc_page *run[RA_MAX];
        uint32_t frames[RA_MAX], n = 0;
        while (n < RA_MAX && idx + n <= ino->dirty_hi) {
            struct pc_page *p = find(id, idx + n);
            if (!p || !(p->flags & P_DIRTY))
                break;
            p->readers++;
            clear_dirty(ino, p);        /* a change while writing marks it again */
            run[n] = p;
            frames[n++] = p->frame;
        }
        if (!n) {
            idx++;
            continue;
        }
        if (ino->fs->writepages(ino, idx, n, frames) < 0) {
            kprintf("pcache: write error, file %lu pages %lu-%lu: changes lost\n",
                    ino->fs_ino, idx, idx + n - 1);
            err = -EIO;
        }
        for (uint32_t k = 0; k < n; k++)
            run[k]->readers--;
        idx += n;
    }
    vfs_put(ino);
    irq_restore(flags);
    return err;
}

int pcache_sync(void)
{
    int err = 0;
    uint32_t flags = irq_save();
    for (int k = 0; dirty_list && k < 4096; k++)   /* bounded: writers may keep at it */
        if (pcache_flush(dirty_list) < 0)
            err = -EIO;
    irq_restore(flags);
    return err;
}

/* --- finding room ---------------------------------------------------------- */

/* The slot a new page goes in: empty, else a clean unpinned one not used
   since the last pass (clearing the bits it passes). If only dirty pages
   are left, write one back first (sleeps). NULL: all four are pinned. */
static struct pc_page *clean_victim(struct pc_page *s)
{
    for (int w = 0; w < WAYS; w++)
        if ((!s[w].id || !s[w].frame) && !s[w].readers)
            return &s[w];
    for (int pass = 0; pass < 2; pass++)
        for (int w = 0; w < WAYS; w++) {
            if (s[w].readers || (s[w].flags & P_DIRTY))
                continue;
            if (!(s[w].flags & P_USED))
                return &s[w];
            s[w].flags &= ~P_USED;
        }
    return NULL;
}

static struct pc_page *victim(struct pc_page *s)
{
    struct pc_page *p = clean_victim(s);
    if (p)
        return p;
    for (int w = 0; w < WAYS; w++)
        if (!s[w].readers && (s[w].flags & P_DIRTY)) {
            pcache_flush(owner[&s[w] - table]);
            return clean_victim(s);     /* once: no loop if the write failed */
        }
    return NULL;
}

/* The page allocator is out of frames: give back up to `want` clean,
   unpinned pages, those not used lately first. Bounded: SCAN_MAX pages
   looked at per round, 4 * SCAN_MAX slots in all (empty ones are a single
   compare). If the first round only took the "used" bits away (everything
   was busy lately), the second takes the pages anyway — the clock already
   gave them their second chance. */
uint32_t pcache_reclaim(uint32_t want)
{
    if (!table)
        return 0;
    uint32_t freed = 0;
    for (int round = 0; round < 2 && !freed; round++) {
        uint32_t looked = 0;
        for (uint32_t k = 0; k < 4 * SCAN_MAX && looked < SCAN_MAX && freed < want; k++) {
            struct pc_page *p = &table[hand];
            hand = (hand + 1) & (nslots - 1);
            if (!p->frame)
                continue;
            looked++;
            if (p->readers || (p->flags & P_DIRTY))
                continue;
            if ((p->flags & P_USED) && round == 0) {
                p->flags &= ~P_USED;
                continue;
            }
            drop(p);
            freed++;
        }
    }
    return freed;
}

/* Put a frame holding page `index` into the table. NULL if no room. */
static struct pc_page *install(uint32_t id, uint32_t index, uint32_t frame)
{
    struct pc_page *p = victim(set_of(id, index));
    if (!p)
        return NULL;
    if (find(id, index))
        return NULL;                    /* someone put it there while we slept */
    if (p->frame)
        pmm_free(p->frame);
    else
        nframes++;
    p->id = id;
    p->index = index;
    p->frame = frame;
    p->flags = 0;
    p->readers = 0;
    return p;
}

/* --- reading ---------------------------------------------------------------- */

/* Read pages [index, index + n) of the file into the cache. Sleeps. */
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

    /* Publish, unless the file's pages were dropped while the disk was busy
       (its id moved on): then these bytes may be old. */
    if (!err && ino->pc_id == id)
        for (uint32_t k = 0; k < got; k++)
            if (install(id, index + k, frames[k]))
                frames[k] = 0;
    for (uint32_t k = 0; k < got; k++)
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

/* --- read-ahead that doesn't wait ------------------------------------------ */

/*
 * The first page of each read-ahead window is marked P_RA. When a reader
 * gets there, the NEXT window goes to the disk at once (blockdev_submit)
 * and the reader goes on copying this one: the disk and the CPU work at
 * the same time. Pages on their way are in the table, pinned by the request
 * and marked P_IO; a reader that gets there first sleeps until the disk is
 * done (io_wait). A failed read drops its pages: the reader then reads them
 * the ordinary way and gets the error.
 */
struct ra_req {
    struct bio bio;
    struct bio_vec v[RA_MAX];
    struct pc_page *pg[RA_MAX];
    uint8_t npg, used;
};
static struct ra_req ra_pool[RA_REQS];
static struct wait_queue io_wait;

/* The disk is done (in its worker thread or its interrupt: short, no sleeping). */
static void ra_done(struct bio *b)
{
    struct ra_req *r = b->priv;
    for (int k = 0; k < r->npg; k++) {
        struct pc_page *p = r->pg[k];
        if (b->status)
            p->id = 0;                  /* never matches: read again, the slow way */
        p->flags &= ~(P_IO | P_RA);
        p->readers--;
    }
    r->used = 0;
    wq_wake_all(&io_wait);
}

static struct ra_req *ra_get(void)
{
    for (int k = 0; k < RA_REQS; k++)
        if (!ra_pool[k].used) {
            ra_pool[k].used = 1;
            ra_pool[k].npg = 0;
            return &ra_pool[k];
        }
    return NULL;
}

/* A reader reached page `at`, marked P_RA: start the window after it. */
static void readahead(struct inode *ino, uint32_t at)
{
    uint32_t index = ino->ra_next;
    if (!ino->fs->map_pages || index <= at || index > at + 2 * RA_MAX || !ino->size)
        return;                         /* not reading in order (any more) */
    uint32_t id = id_of(ino), last = (ino->size - 1) / PAGE_SIZE;
    uint32_t n = min_u32(ino->ra_win ? ino->ra_win * 2u : RA_START, RA_MAX);
    if (index > last)
        return;
    n = min_u32(n, last - index + 1);
    for (uint32_t k = 0; k < n; k++)
        if (find(id, index + k)) {
            n = k;
            break;
        }
    uint32_t lba[RA_MAX];
    uint8_t secs[RA_MAX];
    struct blockdev *dev;
    int got = n ? ino->fs->map_pages(ino, index, n, lba, secs, &dev) : 0;
    if (got <= 0 || ino->pc_id != id)
        return;

    /* Pages next to each other on the disk go in one request. */
    int k = 0, stop = 0, sent = 0;
    while (k < got && !stop) {
        struct ra_req *r = ra_get();
        if (!r)
            break;
        uint32_t first = lba[k];
        while (k < got && r->npg < RA_MAX &&
               (!r->npg || (secs[k - 1] == PAGE_SIZE / SECTOR_SIZE && lba[k] == lba[k - 1] + secs[k - 1]))) {
            uint32_t frame = pmm_alloc();
            struct pc_page *p = frame ? install(id, index + k, frame) : NULL;
            if (!p) {
                if (frame)
                    pmm_free(frame);
                stop = 1;
                break;
            }
            if (secs[k] < PAGE_SIZE / SECTOR_SIZE)      /* the file's last page */
                memset((uint8_t *)P2V(frame) + secs[k] * SECTOR_SIZE, 0,
                       PAGE_SIZE - secs[k] * SECTOR_SIZE);
            p->flags = P_IO | (k == 0 ? P_RA : 0);
            p->readers = 1;             /* pinned by the request */
            r->pg[r->npg] = p;
            r->v[r->npg++] = (struct bio_vec){ frame, secs[k] * SECTOR_SIZE };
            k++;
        }
        if (!r->npg) {
            r->used = 0;
            break;
        }
        r->bio = (struct bio){ .lba = first, .v = r->v, .nv = r->npg, .done = ra_done, .priv = r };
        ra_pages += r->npg;
        if (blockdev_submit(dev, &r->bio) < 0) {
            r->bio.status = -1;
            ra_done(&r->bio);
        }
        sent = 1;
    }
    if (k) {
        ino->ra_next = index + k;
        ino->ra_win = (uint16_t)k;
    }
    if (sent)
        thread_yield();                 /* let the disk's worker start the command */
}

/* A page found in the table: pin it, start the next read-ahead if it is
   marked, and wait if the disk is still filling it. NULL: that read failed
   (look again). */
static struct pc_page *use(struct inode *ino, struct pc_page *p)
{
    p->flags |= P_USED;
    p->readers++;
    if (p->flags & P_RA) {
        p->flags &= ~P_RA;
        readahead(ino, p->index);       /* before waiting: keep the disk busy */
    }
    uint32_t id = p->id;
    while ((p->flags & P_IO) && p->id == id)
        wq_sleep(&io_wait);
    if (p->id != id) {
        p->readers--;
        return NULL;
    }
    hits++;
    return p;
}

/* Page `index` of the file, pinned. `want`: pages the caller is about to
   read (the read-ahead hint). NULL: -EIO / -ENOMEM in *err. */
static struct pc_page *get_read(struct inode *ino, uint32_t index, uint32_t want, int *err)
{
    for (int tries = 0; tries < 8; tries++) {
        uint32_t id = id_of(ino);
        struct pc_page *p = find(id, index);
        if (p) {
            if ((p = use(ino, p)) != NULL)
                return p;
            continue;
        }
        uint32_t n = window(ino, id, index, want);
        *err = fill(ino, index, n);
        if (*err)
            return NULL;
        if (want < n) {                 /* read ahead of the caller: mark where to go on */
            struct pc_page *t = find(id, index + want);
            if (t)
                t->flags |= P_RA;
        }
    }
    *err = -EIO;                        /* writes keep racing us, or no room */
    return NULL;
}

static int copy_out(struct inode *ino, uint32_t pos, void *dst, uint32_t n, int user)
{
    if (!ino->fs || !table)
        return -EIO;
    uint8_t *out = dst;
    uint32_t flags = irq_save();
    int err = 0;
    while (n) {
        uint32_t index = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        uint32_t k = min_u32(PAGE_SIZE - in, n);
        struct pc_page *p = find(id_of(ino), index);
        if (p && !(p->flags & (P_IO | P_RA))) {     /* the plain hit, inline */
            hits++;
            p->flags |= P_USED;
            p->readers++;
        } else if (!(p && (p = use(ino, p))) &&
                   !(p = get_read(ino, index, (pos + n - 1) / PAGE_SIZE - index + 1, &err))) {
            break;
        }
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

/* --- writing ---------------------------------------------------------------- */

struct pc_page *pcache_get_write(struct inode *ino, uint32_t index, int need_old, int *err)
{
    *err = 0;
    if (!table) {
        *err = -EIO;
        return NULL;
    }
    if (need_old)
        return get_read(ino, index, 1, err);
    for (int tries = 0; tries < 8; tries++) {
        uint32_t id = id_of(ino);
        struct pc_page *p = find(id, index);
        if (p) {
            if ((p = use(ino, p)) != NULL)  /* (waits if the disk is filling it) */
                return p;
            continue;
        }
        uint32_t frame = pmm_alloc();   /* replaced whole, or new: no disk read */
        if (!frame && ino->pc_ndirty) {
            pcache_flush(ino);          /* RAM full of our changes: clean them, retry */
            frame = pmm_alloc();
        }
        if (!frame) {
            *err = -ENOMEM;
            return NULL;
        }
        memset(P2V(frame), 0, PAGE_SIZE);
        p = install(id, index, frame);
        if (p) {
            p->flags |= P_USED;
            p->readers++;
            return p;
        }
        pmm_free(frame);
        if (!find(id, index)) {         /* not "it appeared while we slept": no room */
            *err = -ENOMEM;
            return NULL;
        }
    }
    *err = -EIO;
    return NULL;
}

void *pcache_page_data(struct pc_page *p)
{
    return P2V(p->frame);
}

void pcache_put_write(struct inode *ino, struct pc_page *p, int dirty)
{
    if (dirty && p->id == ino->pc_id)
        mark_dirty(ino, p);
    p->readers--;
}

int pcache_too_dirty(void)
{
    return ndirty > dirty_max;
}

/* --- forgetting -------------------------------------------------------------- */

/* Drop the file's pages, dirty ones included (unwritten changes are lost:
   for a file that is deleted, or after pcache_flush). */
void pcache_invalidate(struct inode *ino)
{
    uint32_t flags = irq_save();
    if (table && ino->pc_ndirty) {
        for (uint32_t idx = ino->dirty_lo; ino->pc_ndirty && idx <= ino->dirty_hi; idx++) {
            struct pc_page *p = find(ino->pc_id, idx);
            if (p && (p->flags & P_DIRTY)) {
                clear_dirty(ino, p);
                p->id = 0;              /* the frame is reused or reclaimed */
            }
        }
    }
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

uint32_t pcache_ra_pages(void)
{
    return ra_pages;
}
