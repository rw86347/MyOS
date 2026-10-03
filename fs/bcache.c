/*
 * bcache.c — the block cache: recently used disk blocks, kept in RAM
 *
 * A file system reads its blocks through here (see bcache.h). The first
 * read of a block goes to the disk; after that it's a hash lookup. A
 * changed block is only marked dirty: a background thread writes all
 * changes about once a second, in one batch, followed by ONE flush of
 * the drive's own cache. (Writing every change at once would cost a disk
 * round trip — milliseconds on a real disk — per write() call.) The price:
 * switching the machine off loses up to about a second of changes. `sync`
 * writes everything at once.
 *
 * Speed and predictability (CLAUDE.md rules 1, 6):
 *   - fixed size: nbuf buffers of one page each — an eighth of RAM, at
 *     most NBUF_MAX (8 MiB) — so memory use is known; it holds what a boot
 *     runs (BusyBox, bash and its libraries: ~3 MiB) several times over;
 *     a buffer's page is taken from the page allocator on first use
 *   - finding a block: O(1), a hash on (device, block number)
 *   - finding a buffer to reuse: O(1) in practice, the tail of the LRU
 *     ("least recently used") list — only held buffers are skipped
 *
 * No locks: everything between two sleeps runs with interrupts off (one
 * CPU), so it can't be interrupted halfway. The sleeps are the disk I/O;
 * after each one the code looks at the state again.
 */
#include <bcache.h>
#include <errno.h>
#include <interrupts.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <timer.h>

#define NBUF_MAX   2048         /* 8 MiB of blocks at most */
#define HASH_SIZE  2048         /* buckets: 2^11, see hash_of */
#define FLUSH_MS   1000         /* changes reach the disk within ~1 s */

static struct buf bufs[NBUF_MAX];
static int nbuf;                        /* in use: depends on RAM */
static struct buf *hash[HASH_SIZE];
static struct buf lru;                  /* list head: lru.lru_next = newest */
static struct wait_queue io_done;       /* a read ended, or a buffer came free */
static struct wait_queue flusher_wait;
static uint32_t ndirty, nreads, nwrites;
static int flusher_started;

/* Multiplicative hashing: the top 11 bits of the product mix every bit of
   the block number. */
static uint32_t hash_of(struct blockdev *dev, uint32_t block)
{
    return ((block ^ ((uint32_t)(uintptr_t)dev >> 4)) * 2654435761u) >> 21;
}

static void hash_insert(struct buf *b)
{
    struct buf **head = &hash[hash_of(b->dev, b->block)];
    b->hash_next = *head;
    if (*head)
        (*head)->hash_pprev = &b->hash_next;
    *head = b;
    b->hash_pprev = head;
}

static void hash_remove(struct buf *b)
{
    if (!b->hash_pprev)
        return;                         /* not in the table */
    *b->hash_pprev = b->hash_next;
    if (b->hash_next)
        b->hash_next->hash_pprev = b->hash_pprev;
    b->hash_pprev = NULL;
}

/* Move b to the front of the LRU list: just used. */
static void lru_touch(struct buf *b)
{
    b->lru_prev->lru_next = b->lru_next;
    b->lru_next->lru_prev = b->lru_prev;
    b->lru_next = lru.lru_next;
    b->lru_prev = &lru;
    lru.lru_next->lru_prev = b;
    lru.lru_next = b;
}

static struct buf *lookup(struct blockdev *dev, uint32_t block, uint32_t size)
{
    for (struct buf *b = hash[hash_of(dev, block)]; b; b = b->hash_next)
        if (b->dev == dev && b->block == block && b->size == size)
            return b;
    return NULL;
}

/* The least recently used buffer nobody holds, or NULL. */
static struct buf *victim(void)
{
    for (struct buf *b = lru.lru_prev; b != &lru; b = b->lru_prev)
        if (b->refs == 0)
            return b;
    return NULL;
}

static uint32_t sectors(const struct buf *b) { return b->size / SECTOR_SIZE; }

/* Write one changed block to the disk (sleeps). A change made while the
   write is running marks it dirty again, so it's written next time. A
   failed write is reported and dropped: retrying a broken disk forever
   would hang every program waiting for a buffer. */
static int write_back(struct buf *b)
{
    b->refs++;                          /* not reused while being written */
    b->dirty = 0;
    ndirty--;
    int err = blockdev_write(b->dev, b->block * sectors(b), sectors(b), b->data);
    nwrites++;
    if (err)
        kprintf("bcache: write error on %s, block %lu: change lost\n",
                b->dev->name, b->block);
    b->refs--;
    wq_wake_all(&io_done);              /* it may be free to reuse now */
    return err;
}

/* (Inlined into both callers: bread is the hot one — every block a file
   system touches goes through it — and the flag then costs nothing.) */
static inline __attribute__((always_inline))
struct buf *get(struct blockdev *dev, uint32_t block, uint32_t size, int read);

struct buf *bread(struct blockdev *dev, uint32_t block, uint32_t size)
{
    return get(dev, block, size, 1);
}

struct buf *bclear(struct blockdev *dev, uint32_t block, uint32_t size)
{
    struct buf *b = get(dev, block, size, 0);
    if (b)
        memset(b->data, 0, size);
    return b;
}

static inline __attribute__((always_inline))
struct buf *get(struct blockdev *dev, uint32_t block, uint32_t size, int read)
{
    uint32_t flags = irq_save();
    struct buf *b;
    for (;;) {
        b = lookup(dev, block, size);
        if (b) {                        /* cached (or being read right now) */
            b->refs++;
            lru_touch(b);
            while (!b->valid && b->dev)
                wq_sleep(&io_done);     /* someone else's read is running */
            if (b->valid)
                break;
            b->refs--;                  /* their read failed */
            b = NULL;
            break;
        }
        b = victim();
        if (!b) {
            wq_sleep(&io_done);         /* every buffer is held: wait */
            continue;
        }
        if (b->dirty) {
            write_back(b);              /* slept: look at everything again */
            continue;
        }
        if (!b->data) {                 /* first use of this buffer */
            uint32_t frame = pmm_alloc();
            if (!frame) {
                b = NULL;
                break;
            }
            b->data = P2V(frame);
        }
        hash_remove(b);
        b->dev = dev;
        b->block = block;
        b->size = size;
        b->valid = 0;
        b->refs = 1;
        hash_insert(b);                 /* others asking now wait for us */
        lru_touch(b);
        int err = 0;
        if (read) {
            err = blockdev_read(dev, block * sectors(b), sectors(b), b->data);
            nreads++;
        }
        if (err) {
            hash_remove(b);
            b->dev = NULL;              /* tells waiters it failed */
            b->refs--;
            b = NULL;
        } else {
            b->valid = 1;
        }
        wq_wake_all(&io_done);
        break;
    }
    irq_restore(flags);
    return b;
}

static void flusher(void *arg);

void bdirty(struct buf *b)
{
    uint32_t flags = irq_save();
    if (!b->dirty) {
        b->dirty = 1;
        ndirty++;
    }
    /* The background writer starts with the first change: a machine that
       only ever reads its disks doesn't need it. */
    if (!flusher_started) {
        flusher_started = 1;
        if (!thread_create("flush", flusher, NULL))
            kprintf("bcache: no flusher thread: changes are written only by sync\n");
    }
    irq_restore(flags);
}

void brelse(struct buf *b)
{
    uint32_t flags = irq_save();
    if (--b->refs == 0)
        wq_wake_all(&io_done);          /* someone may wait for a free buffer */
    irq_restore(flags);
}

int bsync(void)
{
    uint32_t flags = irq_save();
    struct blockdev *written[BLOCKDEV_MAX];
    int nwritten = 0, err = 0;
    for (int i = 0; i < nbuf && ndirty; i++) {
        struct buf *b = &bufs[i];
        if (!b->dirty)
            continue;
        struct blockdev *disk = b->dev;
        while (disk->parent)
            disk = disk->parent;        /* flush the drive, not the partition */
        int seen = 0;
        for (int k = 0; k < nwritten; k++)
            seen |= written[k] == disk;
        if (!seen && nwritten < BLOCKDEV_MAX)
            written[nwritten++] = disk;
        if (write_back(b) < 0)
            err = -EIO;
    }
    for (int k = 0; k < nwritten; k++)  /* one drive-cache flush per disk */
        if (blockdev_flush(written[k]) < 0)
            err = -EIO;
    irq_restore(flags);
    return err;
}

/* The background writer: every FLUSH_MS, write whatever has changed. */
static void flusher(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = irq_save();
        uint32_t deadline = timer_ms() + FLUSH_MS;
        wq_sleep_until(&flusher_wait, deadline ? deadline : 1);
        irq_restore(flags);
        if (ndirty)
            bsync();
    }
}

int bcache_size(void) { return nbuf; }

uint32_t bcache_pages(void)
{
    uint32_t n = 0;
    for (int i = 0; i < nbuf; i++)
        n += bufs[i].data != NULL;
    return n;
}

void bcache_stats(uint32_t *reads, uint32_t *writes)
{
    *reads = nreads;
    *writes = nwrites;
}

void bcache_init(void)
{
    nbuf = pmm_usable_count() / 8;      /* an eighth of RAM, in pages */
    if (nbuf > NBUF_MAX)
        nbuf = NBUF_MAX;
    if (nbuf < 64)
        nbuf = 64;
    lru.lru_next = lru.lru_prev = &lru;
    for (int i = 0; i < nbuf; i++) {
        bufs[i].lru_next = &lru;        /* append: all start unused */
        bufs[i].lru_prev = lru.lru_prev;
        lru.lru_prev->lru_next = &bufs[i];
        lru.lru_prev = &bufs[i];
    }
}
