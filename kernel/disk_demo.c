/*
 * disk_demo.c — exercise the disk + file system drivers at boot.
 *
 * Finds every disk, reads its partition table, mounts FAT32 partitions,
 * prints the whole directory tree, then runs self-tests
 * (reads across cluster boundaries, long names, ATA write + read-back).
 *
 * Temporary: once we have a VFS, syscalls and a shell, `ls` and `cat`
 * will do this job instead.
 */
#include <blockdev.h>
#include <disk_demo.h>
#include <fat32.h>
#include <kprintf.h>
#include <selftest.h>
#include <string.h>

/* ---- directory tree ----------------------------------------------------- */

struct tree_ctx {
    struct fat32_fs *fs;
    int depth;
};

static int tree_cb(const struct fat32_dirent *e, void *ctx)
{
    struct tree_ctx *t = ctx;

    for (int i = 0; i < t->depth; i++)
        kprintf("    ");
    if (e->attr & FAT_ATTR_DIRECTORY) {
        kprintf("%s/\n", e->name);
        struct tree_ctx sub = { t->fs, t->depth + 1 };
        if (t->depth < 8)               /* guard against runaway recursion */
            fat32_readdir(t->fs, e, tree_cb, &sub);
    } else {
        kprintf("%-32s %6lu bytes\n", e->name, e->size);
    }
    return 0;                           /* keep going */
}

/* ---- self-tests --------------------------------------------------------- */

static void run_tests(struct fat32_fs *fs)
{
    static uint8_t buf[4096];
    struct fat32_dirent f;

    static const char hello[] = "Hello from a FAT32 USB stick!\n";
    int n0 = fat32_lookup(fs, "/HELLO.TXT", &f) == 0 ? fat32_read(fs, &f, 0, buf, sizeof buf) : -1;
    check(n0 == (int)sizeof hello - 1 && memcmp(buf, hello, n0) == 0, "read /HELLO.TXT");

    /* numbers.txt: 100 lines of exactly 24 bytes: "line 001 of numbers.txt\n" */
    int ok = fat32_lookup(fs, "/data/numbers.txt", &f) == 0 && f.size == 2400;
    check(ok, "lookup /data/numbers.txt (2400 bytes)");
    if (ok) {
        int n = fat32_read(fs, &f, 0, buf, sizeof buf);
        check(n == 2400 && memcmp(buf + 99 * 24, "line 100 of numbers.txt\n", 24) == 0,
              "read whole file, last line correct");
        /* Line 42 starts at byte 41*24 = 984: inside cluster 2 of the file. */
        n = fat32_read(fs, &f, 41 * 24, buf, 24);
        check(n == 24 && memcmp(buf, "line 042 of numbers.txt\n", 24) == 0,
              "read at offset 984 (line 42)");
    }

    /* pattern.bin: 65536 bytes where byte i == i % 256. 128 clusters long,
       read in odd-sized pieces so reads straddle sector/cluster edges. */
    ok = fat32_lookup(fs, "/data/pattern.bin", &f) == 0 && f.size == 65536;
    check(ok, "lookup /data/pattern.bin (65536 bytes)");
    if (ok) {
        uint32_t off = 0, bad = 0;
        int n;
        while ((n = fat32_read(fs, &f, off, buf, 1000)) > 0) {
            for (int i = 0; i < n; i++)
                if (buf[i] != (uint8_t)(off + i))
                    bad++;
            off += n;
        }
        check(off == 65536 && bad == 0, "read 64 KiB across 128 clusters, all bytes match");
    }

    check(fat32_lookup(fs, "/DOCS/NOTES.TXT", &f) == 0, "case-insensitive lookup");
    check(fat32_lookup(fs, "/docs/This is a long file name.txt", &f) == 0 && f.size == 52,
          "long file name lookup");
    check(fat32_lookup(fs, "/no/such/file", &f) < 0, "missing file is not found");
    check(fat32_lookup(fs, "/HELLO.TXT/x", &f) < 0, "file used as a directory fails");

    /* ATA write test: sector 1 of the disk lies in the unused gap between
       the MBR and the first partition (which starts at sector 2048), so we
       can scribble there safely. Write a pattern, read it back, restore. */
    struct blockdev *disk = fs->dev->parent;
    if (disk && fs->dev->offset > 1) {
        static uint8_t saved[SECTOR_SIZE], pattern[SECTOR_SIZE];
        for (int i = 0; i < SECTOR_SIZE; i++)
            pattern[i] = (uint8_t)(i * 7 + 3);
        ok = blockdev_read(disk, 1, 1, saved) == 0
          && blockdev_write(disk, 1, 1, pattern) == 0
          && blockdev_read(disk, 1, 1, buf) == 0
          && memcmp(buf, pattern, SECTOR_SIZE) == 0;
        ok = ok && blockdev_write(disk, 1, 1, saved) == 0;
        check(ok, "ATA write sector + read back + restore");
    }
}

/* ---- entry point -------------------------------------------------------- */

void disk_demo(void)
{
    /* (The disks and their partitions were found at boot: storage_init.)
       Try to mount every partition as FAT32. */
    static struct fat32_fs fs;
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *dev = blockdev_get(i);
        if (!dev->parent || fat32_mount(&fs, dev) < 0)
            continue;

        kprintf("fat32: mounted %s, label \"%s\", %lu clusters of %lu bytes. Files:\n",
                dev->name, fs.label, fs.total_clusters,
                fs.sectors_per_cluster * SECTOR_SIZE);

        struct fat32_dirent root;
        fat32_lookup(&fs, "/", &root);
        struct tree_ctx t = { &fs, 1 };
        fat32_readdir(&fs, &root, tree_cb, &t);

        run_tests(&fs);
        return;                 /* one file system is enough for the demo */
    }
    kprintf("No FAT32 partition found.\n");
}
