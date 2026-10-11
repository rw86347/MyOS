/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef BCACHE_H
#define BCACHE_H

#include <blockdev.h>
#include <stdint.h>

/*
 * The block cache: recently used disk blocks, kept in RAM (fs/bcache.c).
 * File systems read and change their blocks only through here.
 *
 *   b = bread(dev, n, size)   block n (size bytes) — from RAM, or read now
 *   ... use b->data ...
 *   bdirty(b)                 "I changed it": it's written back later
 *   brelse(b)                 done with it (it stays cached)
 *   bsync()                   write every changed block now
 *
 * A held buffer (between bread and brelse) is never reused for another
 * block. These may sleep while the disk works: call them from syscalls
 * and kernel threads, never from an interrupt handler.
 */
struct buf {
    struct blockdev *dev;
    uint32_t block;             /* block number, in units of `size` */
    uint32_t size;              /* 512 ... 4096 bytes */
    uint8_t *data;              /* the block's bytes (one page) */

    /* the cache's own bookkeeping */
    uint16_t refs;              /* holders right now */
    uint8_t  valid;             /* data holds the block (read finished) */
    uint8_t  dirty;             /* changed since it was last written */
    struct buf *hash_next, **hash_pprev;
    struct buf *lru_prev, *lru_next;
};

void bcache_init(void);         /* after the disks are found */

struct buf *bread(struct blockdev *dev, uint32_t block, uint32_t size);  /* NULL: I/O error */
/* Like bread, for a block whose old contents don't matter (it was just
   allocated, or will be overwritten whole): zeros, and no disk read. */
struct buf *bclear(struct blockdev *dev, uint32_t block, uint32_t size);
/* The block now holds file data, which lives in the page cache: drop any
   cached copy (a freed directory or indirect block), dirty or not, so it
   is never written over the data or read instead of it. */
void bforget(struct blockdev *dev, uint32_t block, uint32_t size);
void bdirty(struct buf *b);
void brelse(struct buf *b);
int  bsync(void);               /* 0, or -EIO */
void bcache_drop(struct blockdev *disk);    /* forget a disk and its partitions */

/* For tests and benchmarks: disk reads and writes done so far, and pages
   the cache holds. */
void bcache_stats(uint32_t *reads, uint32_t *writes);
uint32_t bcache_pages(void);
int bcache_size(void);          /* buffers (an eighth of RAM, at most 2048) */

#endif
