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
}
