/*
 * ext2test.c — the disk file system, against files Linux made (step 9b)
 *
 * The test disk is made by Linux's own mke2fs from a directory (see the
 * Makefile): hello.txt, link -> hello.txt, dir/sub/nested.txt and
 * numbers.txt (1..50000, one per line — with 1 KiB blocks it needs
 * indirect and double-indirect blocks). We read them back, check /data
 * really is another file system, and leave persist.txt behind. After QEMU
 * stops, the Makefile reads persist.txt with Linux's debugfs and checks
 * the whole disk with e2fsck.
 *
 * Exits 0 if all is well, else the number of the first failed check.
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("ext2test: check ");                        \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

#define SYS_sync     36
#define SYS_ftruncate 93
#define SYS_rename   38
#define SYS_rmdir    40
#define SYS_statfs64 268
#define EBUSY 16
#define EXDEV 18

static int file_is(const char *path, const char *want, int len)
{
    char b[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    long n = read(fd, b, sizeof b);
    close(fd);
    return n == len && memcmp(b, want, len) == 0;
}

static char buf[1000];

/* Section 6's file; tools/check-pattern.py computes the same bytes. */
static uint8_t pattern(uint32_t pos) { return (uint8_t)((pos ^ (pos >> 9)) * 31); }

static uint8_t expect_at(uint32_t pos)
{
    if (pos == 200000)
        return 'Z';
    if (pos >= 123457)
        return 0;
    if (pos >= 5000 && pos < 14000)
        return 0xAA;
    return pattern(pos);
}

int main(void)
{
    struct statx st, root;

    /* 1. Small files, a symlink, a nested directory. */
    CHECK(file_is("/data/hello.txt", "hello from ext2\n", 16));
    CHECK(file_is("/data/link", "hello from ext2\n", 16));
    CHECK(file_is("/data/dir/sub/nested.txt", "nested\n", 7));

    /* 2. numbers.txt, read 1000 bytes at a time: exactly 1, 2, ... 50000. */
    int fd = open("/data/numbers.txt", O_RDONLY);
    CHECK(fd >= 3);
    uint32_t expect = 1, cur = 0;
    int in_order = 1;
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                in_order &= cur == expect;
                expect++;
                cur = 0;
            } else {
                cur = cur * 10 + (uint32_t)(buf[i] - '0');
            }
        }
    }
    close(fd);
    CHECK(n == 0 && in_order && expect == 50001);

    /* 3. It's another file system: its own device number, ext2's magic,
          and the lost+found directory mke2fs makes. */
    CHECK(statx(AT_FDCWD, "/data/hello.txt", 0, STATX_BASIC_STATS, &st) == 0);
    CHECK(statx(AT_FDCWD, "/bin/busybox", 0, STATX_BASIC_STATS, &root) == 0);
    CHECK(st.stx_dev_minor != root.stx_dev_minor);
    unsigned fs[21];
    CHECK(syscall3(SYS_statfs64, (long)"/data", 84, (long)fs) == 0 && fs[0] == 0xEF53);
    CHECK(statx(AT_FDCWD, "/data/lost+found", 0, STATX_BASIC_STATS, &st) == 0 &&
          (st.stx_mode & S_IFMT) == S_IFDIR);

    /* 4. The mount point stays put; files can't be renamed across. */
    CHECK(syscall3(SYS_rmdir, (long)"/data", 0, 0) == -EBUSY);
    CHECK(syscall3(SYS_rename, (long)"/data", (long)"/data2", 0) == -EBUSY);
    CHECK(syscall3(SYS_rename, (long)"/data/hello.txt", (long)"/tmp/x", 0) == -EXDEV);

    /* 5. Leave a file behind for the Makefile (debugfs) to find. */
    fd = (int)syscall3(SYS_open, (long)"/data/persist.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd >= 3 && write(fd, "written by MyOS\n", 16) == 16);
    close(fd);
    CHECK(syscall3(SYS_sync, 0, 0, 0) == 0);

    /* 6. The page cache write path: 300 000 bytes in odd 777-byte pieces
          (pages split across writes), overwrite a span, cut the file at an
          odd size, write one byte far past the end (a gap that must read
          as zeros). The Makefile checks the same bytes on the disk. */
    fd = (int)syscall3(SYS_open, (long)"/data/pattern.bin", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 3);
    int ok = 1;
    for (uint32_t pos = 0; pos < 300000 && ok; pos += 777) {
        uint32_t k = 300000 - pos < 777 ? 300000 - pos : 777;
        for (uint32_t i = 0; i < k; i++)
            buf[i] = pattern(pos + i);
        ok = write(fd, buf, k) == (long)k;
    }
    CHECK(ok);
    memset(buf, 0xAA, sizeof buf);
    CHECK(lseek(fd, 5000, 0) == 5000);
    for (int i = 0; i < 9; i++)
        ok &= write(fd, buf, 1000) == 1000;
    CHECK(ok);
    CHECK(syscall3(SYS_ftruncate, fd, 123457, 0) == 0);
    CHECK(lseek(fd, 200000, 0) == 200000 && write(fd, "Z", 1) == 1);
    CHECK(lseek(fd, 0, 0) == 0);
    uint32_t pos = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++, pos++)
            ok &= (uint8_t)buf[i] == expect_at(pos);
    }
    CHECK(n == 0 && ok && pos == 200001);
    close(fd);
    CHECK(syscall3(SYS_sync, 0, 0, 0) == 0);

    put("ext2test: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
