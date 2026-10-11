/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pcache_test.c — the page cache (make test, step 9c-2)
 *
 * /data/numbers.txt is a multi-page file Linux's mke2fs put on the test
 * disk. The first copy reads a read-ahead window of pages (4, or the whole
 * file if it is shorter) with one disk read. The second copy, and a copy of
 * a page inside that window, must not touch the disk again. Dropping the
 * file's pages makes the next copy miss, which is what a write does
 * (ext2_write calls the same pcache_invalidate).
 */
#include <kprintf.h>
#include <pcache.h>
#include <selftest.h>
#include <vfs.h>

void pcache_selftest(void)
{
    struct inode *ino;
    if (vfs_lookup("/data/numbers.txt", &ino) < 0) {
        check(0, "pcache: /data/numbers.txt is on the test disk");
        return;
    }
    uint32_t pages = (ino->size + 4095) / 4096, ra = pages < 4 ? pages : 4;

    uint32_t h, m0, m;
    pcache_stats(NULL, &m0);
    uint8_t buf[16];
    int err = file_copy_out(ino, 0, buf, sizeof buf);
    pcache_stats(&h, &m);
    check(err == 0 && buf[0] == '1' && buf[1] == '\n', "pcache: file bytes");
    check(pages >= 2 && m == m0 + ra, "pcache: first touch reads a read-ahead window");

    err = file_copy_out(ino, 0, buf, sizeof buf);
    uint32_t h1, m1;
    pcache_stats(&h1, &m1);
    check(err == 0 && m1 == m, "pcache: second read comes from RAM");
    if (err == 0 && m1 == m)
        kprintf("pcache: second read comes from RAM\n");

    uint32_t h2;
    err = file_copy_out(ino, 4096, buf, 8);
    pcache_stats(&h2, &m);
    check(err == 0 && h2 == h1 + 1 && m == m1,
          "pcache: read-ahead filled the next page");
    if (err == 0 && h2 == h1 + 1 && m == m1)
        kprintf("pcache: read-ahead filled the next page\n");

    pcache_invalidate(ino);
    err = file_copy_out(ino, 0, buf, sizeof buf);
    pcache_stats(&h, &m);
    check(err == 0 && buf[0] == '1' && m > m1,
          "pcache: invalidate drops the cached pages");

    /* The whole file in order, a page at a time: after the first window
       the next ones are read while we copy (P_RA), and every number is
       there, in order (1\n2\n...). */
    pcache_invalidate(ino);
    uint32_t ra0 = pcache_ra_pages(), expect = 1, cur = 0;
    int in_order = 1;
    static uint8_t page[4096];
    for (uint32_t pos = 0; pos < ino->size && in_order; pos += sizeof page) {
        uint32_t k = ino->size - pos < sizeof page ? ino->size - pos : sizeof page;
        if (file_copy_out(ino, pos, page, k) < 0) {
            in_order = 0;
            break;
        }
        for (uint32_t i = 0; i < k; i++) {
            if (page[i] == '\n') {
                in_order &= cur == expect++;
                cur = 0;
            } else {
                cur = cur * 10 + (page[i] - '0');
            }
        }
    }
    uint32_t ahead = pcache_ra_pages() - ra0;
    check(in_order && expect == 50001, "pcache: whole file in order, with read-ahead");
    check(ahead > 0, "pcache: read-ahead ran without waiting");
    if (in_order && ahead > 0)
        kprintf("pcache: %lu of %lu pages read ahead without waiting\n", ahead, pages);
}
