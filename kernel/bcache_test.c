/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * bcache_test.c — self-tests for the block cache (make test)
 *
 * Uses QEMU's second disk (hdb, the FAT32 test disk): between its MBR
 * (sector 0) and its first partition (sector 2048) is unused space we
 * may scribble on.
 */
#include <bcache.h>
#include <blockdev.h>
#include <selftest.h>
#include <string.h>

void bcache_selftest(void)
{
    struct blockdev *disk = NULL;
    for (int i = 0; i < blockdev_count(); i++)
        if (strcmp(blockdev_get(i)->name, "hdb") == 0)
            disk = blockdev_get(i);
    if (!disk) {
        check(0, "bcache: test disk hdb found");
        return;
    }

    /* 1. A block read twice: the second time from RAM. (First, write out
          whatever else is waiting — the mounted disk's superblock.) */
    uint32_t r0, w0, r, w;
    bsync();
    bcache_stats(&r0, &w0);
    struct buf *b = bread(disk, 1, 1024);           /* sectors 2-3 */
    check(b && b->size == 1024, "bcache: read a block");
    if (!b)
        return;
    brelse(b);
    struct buf *again = bread(disk, 1, 1024);
    bcache_stats(&r, &w);
    check(again == b && r == r0 + 1, "bcache: second read comes from RAM");

    /* 2. Change it, sync, and look at the sectors behind the cache's back. */
    static uint8_t saved[1024], raw[1024];
    memcpy(saved, again->data, sizeof saved);
    for (int i = 0; i < 1024; i++)
        again->data[i] = (uint8_t)(i * 13 + 5);
    bdirty(again);
    check(bsync() == 0, "bcache: sync");
    bcache_stats(&r, &w);
    check(w == w0 + 1, "bcache: sync wrote exactly the changed block");
    check(blockdev_read(disk, 2, 2, raw) == 0 && memcmp(raw, again->data, 1024) == 0,
          "bcache: the change is on the disk");
    memcpy(again->data, saved, sizeof saved);       /* put it back */
    bdirty(again);
    brelse(again);
    check(bsync() == 0, "bcache: restore the block");

    /* 3. More blocks than buffers: the least recently used are reused, and
          a reused block comes back from the disk, intact. (Only reading, so
          going past the gap into the partition is fine.) */
    for (uint32_t blk = 2; blk < 2 + (uint32_t)bcache_size() + 50; blk++) {
        struct buf *x = bread(disk, blk, 1024);
        if (!x) {
            check(0, "bcache: read 300 blocks");
            return;
        }
        brelse(x);
    }
    bcache_stats(&r0, &w0);
    b = bread(disk, 1, 1024);
    bcache_stats(&r, &w);
    check(b && r == r0 + 1 && memcmp(b->data, saved, sizeof saved) == 0,
          "bcache: more blocks than buffers (LRU reuse)");
    if (b)
        brelse(b);
    check(bread(disk, disk->sectors, 1024) == NULL, "bcache: reading past the end fails");
}
