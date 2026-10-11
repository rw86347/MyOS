/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * files.c — step 7's test: open / read / close / stat on initramfs files
 *
 * Uses only real Linux system calls (open, read, lseek, close, statx), the
 * same ones musl's fopen/fread/stat make, so passing here means real
 * programs can read their files.
 *
 * Exits 0 if everything worked, otherwise with the number of the first
 * check that failed (and prints which).
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("files: check ");                           \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[1024];
    struct statx st;
    long n;
    int fd;

    /* 1. A small text file: open, read it all, read again at the end. */
    fd = open("/README.txt", O_RDONLY);
    CHECK(fd >= 3);                             /* 0-2 are the console */
    n = read(fd, buf, sizeof buf);
    CHECK(n > 0 && n < (long)sizeof buf);
    CHECK(memcmp(buf, "This file came from the initramfs.", 34) == 0);
    CHECK(read(fd, buf, 10) == 0);              /* end of file */

    /* 2. stat — by name and on the open file (that's how fstat works). */
    CHECK(statx(AT_FDCWD, "/README.txt", 0, STATX_BASIC_STATS, &st) == 0);
    CHECK((st.stx_mode & S_IFMT) == S_IFREG && st.stx_size == (uint64_t)n);
    CHECK(statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &st) == 0);
    CHECK(st.stx_size == (uint64_t)n);
    CHECK(statx(AT_FDCWD, "/test", 0, STATX_BASIC_STATS, &st) == 0);
    CHECK((st.stx_mode & S_IFMT) == S_IFDIR);

    /* 3. lseek, then read from the middle. */
    CHECK(lseek(fd, 5, SEEK_SET) == 5);
    CHECK(read(fd, buf, 4) == 4 && memcmp(buf, "file", 4) == 0);

    /* 4. close, and using the fd afterwards. */
    CHECK(close(fd) == 0);
    CHECK(close(fd) == -EBADF);
    CHECK(read(fd, buf, 1) == -EBADF);

    /* 5. Errors. */
    CHECK(open("/nope", O_RDONLY) == -ENOENT);
    CHECK(open("/README.txt/x", O_RDONLY) == -ENOTDIR);
    CHECK(open("/test", O_WRONLY) == -EISDIR);
    CHECK(lseek(1, 0, SEEK_SET) == -ESPIPE);    /* the console can't seek */

    /* 6. A path with "..", "." and no leading "/". */
    fd = open("test/../test/./numbers.txt", O_RDONLY);
    CHECK(fd >= 3);

    /* 7. Read the 110 KB file in 1000-byte pieces: it must be exactly the
          numbers 1 to 20000, one per line. That also proves every 32 KiB
          chunk the boot loader copied landed in the right place. */
    uint32_t expect = 1, cur = 0;
    int in_order = 1;
    long total = 0;
    while ((n = read(fd, buf, 1000)) > 0) {
        total += n;
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (cur != expect)
                    in_order = 0;
                expect++;
                cur = 0;
            } else {
                cur = cur * 10 + (uint32_t)(buf[i] - '0');
            }
        }
    }
    CHECK(n == 0 && in_order && expect == 20001);
    CHECK(statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &st) == 0);
    CHECK(st.stx_size == (uint64_t)total);

    /* 8. Bad buffers: nothing mapped there, the kernel's memory, and our
          own code (read-only — the kernel must not write it either). */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, (void *)0x10000000, 10) == -EFAULT);
    CHECK(read(fd, (void *)0xC0000000, 10) == -EFAULT);
    CHECK(read(fd, (void *)main, 10) == -EFAULT);
    CHECK(close(fd) == 0);

    put("files: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
