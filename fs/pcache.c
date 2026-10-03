/*
 * pcache.c — a small page cache for disk files (step 9c-2)
 *
 * Direct-mapped: the slot is a hash of the inode and the page number, so a
 * lookup is one array index, never a search. 128 pages is 512 KiB. A new
 * page that lands on a full slot replaces it. That is the right size for
 * the hot pages of a program and its libraries; the block cache underneath
 * still holds the disk blocks.
 *
 * A reader pins the slot while it copies, so a write's invalidate cannot
 * free the frame under the copy. The pin is only the copy, not the disk read.
 */
#include <errno.h>
#include <interrupts.h>
#include <memlayout.h>
#include <paging.h>
#include <pcache.h>
#include <pmm.h>
#include <string.h>
#include <vfs.h>

#define PCACHE_SLOTS 128

struct pc_slot {
    struct inode *ino;
    uint32_t index;
    uint32_t frame;                 /* physical; 0 = empty */
    uint16_t readers;               /* copies in progress */
    uint8_t dead;                   /* invalidated; free when readers hit 0 */
};

static struct pc_slot slots[PCACHE_SLOTS];
static uint32_t hits, misses;

static struct pc_slot *slot_for(struct inode *ino, uint32_t index)
{
    uint32_t h = (uint32_t)(uintptr_t)ino ^ (index * 0x9E3779B9u);
    return &slots[h & (PCACHE_SLOTS - 1)];
}

/* Drop a dead slot's frame once nobody is copying it. Caller holds irqs off. */
static void reap(struct pc_slot *s)
{
    if (s->dead && s->readers == 0 && s->frame) {
        pmm_free(s->frame);
        s->frame = 0;
        s->ino = NULL;
        s->dead = 0;
    }
}

/* Read one page from the file system into a fresh frame and publish it.
   NULL if there is no memory or the disk read failed. */
static struct pc_slot *fill(struct inode *ino, uint32_t index)
{
    struct pc_slot *s = slot_for(ino, index);
    uint32_t flags = irq_save();
    if (!s->dead && s->ino == ino && s->index == index && s->frame) {
        hits++;
        irq_restore(flags);
        return s;
    }
    irq_restore(flags);

    misses++;
    uint32_t frame = pmm_alloc();
    if (!frame)
        return NULL;
    memset(P2V(frame), 0, PAGE_SIZE);

    uint32_t off = index * PAGE_SIZE;
    uint32_t n = 0;
    if (off < ino->size)
        n = ino->size - off < PAGE_SIZE ? ino->size - off : PAGE_SIZE;
    if (n && ino->fs->read_kernel(ino, off, P2V(frame), n) < 0) {
        pmm_free(frame);
        return NULL;
    }

    flags = irq_save();
    if (!s->dead && s->ino == ino && s->index == index && s->frame) {
        /* Someone else filled it while we were reading. Theirs stands. */
        irq_restore(flags);
        pmm_free(frame);
        return s;
    }
    uint32_t old = s->frame;
    struct inode *old_ino = s->ino;
    uint8_t old_dead = s->dead;
    uint16_t old_readers = s->readers;
    if (old && old_readers) {
        /* The slot is being copied. Leave it dead; it reaps itself. We
           need a different slot, so this fill is abandoned. */
        irq_restore(flags);
        pmm_free(frame);
        (void)old_ino;
        (void)old_dead;
        return NULL;
    }
    if (old)
        pmm_free(old);
    s->ino = ino;
    s->index = index;
    s->frame = frame;
    s->readers = 0;
    s->dead = 0;
    irq_restore(flags);
    return s;
}

int pcache_read(struct inode *ino, uint32_t pos, void *dst, uint32_t n)
{
    if (!ino->fs)
        return -EIO;
    uint8_t *out = dst;
    int ahead = 1;                      /* one read-ahead per call */
    while (n) {
        uint32_t index = pos / PAGE_SIZE;
        uint32_t in = pos % PAGE_SIZE;
        uint32_t k = PAGE_SIZE - in;
        if (k > n)
            k = n;

        struct pc_slot *s = fill(ino, index);
        if (!s)
            return -EIO;

        uint32_t flags = irq_save();
        if (s->dead || s->ino != ino || s->index != index || !s->frame) {
            irq_restore(flags);
            return -EIO;
        }
        s->readers++;
        uint32_t frame = s->frame;
        irq_restore(flags);

        memcpy(out, (uint8_t *)P2V(frame) + in, k);

        flags = irq_save();
        s->readers--;
        reap(s);
        irq_restore(flags);

        /* The next page, so the next fault is a copy from RAM. */
        if (ahead && (index + 1) * PAGE_SIZE < ino->size) {
            ahead = 0;
            struct pc_slot *nslot = fill(ino, index + 1);
            (void)nslot;
        }

        out += k;
        pos += k;
        n -= k;
    }
    return 0;
}

void pcache_invalidate(struct inode *ino)
{
    uint32_t flags = irq_save();
    for (int i = 0; i < PCACHE_SLOTS; i++) {
        struct pc_slot *s = &slots[i];
        if (s->ino != ino || !s->frame)
            continue;
        s->dead = 1;
        reap(s);
    }
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
    uint32_t n = 0;
    for (int i = 0; i < PCACHE_SLOTS; i++)
        if (slots[i].frame)
            n++;
    return n;
}
