/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * streambench.c — wall-clock time to read a file once, in order (make bench)
 *
 * diskbench counts CPU instructions; this measures what a user waits for.
 * It reads two 8 MiB files nobody has touched since boot, 64 KiB at a time:
 *
 *   big4: and does nothing with them — the disk's streaming speed;
 *   big3: and does a little work on every byte (a checksum, like `wc` or
 *         `cksum`) — read-ahead that doesn't wait overlaps the disk with it.
 *
 * Only meaningful without QEMU's -icount (tools/streambench.sh): with it,
 * waiting for the disk takes no time at all.
 *
 * argv[1]: the directory holding big3 and big4.
 */
#include <mini.h>

#define SYS_clock_gettime 265
#define CHUNK   65536

static uint8_t buf[CHUNK] __attribute__((aligned(4096)));

static uint64_t now_us(void)
{
    long ts[2];
    syscall3(SYS_clock_gettime, 1, (long)ts, 0);        /* CLOCK_MONOTONIC */
    return (uint64_t)ts[0] * 1000000 + (uint64_t)ts[1] / 1000;
}

static void report(const char *what, uint64_t bytes, uint64_t us)
{
    put("streambench: ");
    put(what);
    put_u64(us / 1000);
    put(" ms, ");
    put_u64(us ? bytes * 1000000 / us / 1024 : 0);
    put(" KiB/s\n");
}

/* Read the whole file; work != 0: also checksum every byte. */
static int stream(const char *path, int work, uint64_t *bytes, uint32_t *sum)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n;
    uint32_t s = 0;
    *bytes = 0;
    while ((n = read(fd, buf, CHUNK)) > 0) {
        *bytes += (uint64_t)n;
        if (work)
            for (long i = 0; i < n; i++)
                s = (s << 5) + s + buf[i];      /* ~4 instructions a byte */
    }
    close(fd);
    *sum = s;
    return n < 0 ? -1 : 0;
}

static char path[128];

static void join(const char *dir, const char *name)
{
    size_t d = strlen(dir), k = strlen(name);
    memcpy(path, dir, d);
    path[d] = '/';
    memcpy(path + d + 1, name, k + 1);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/data";
    uint64_t bytes, t;
    uint32_t sum;

    /* Read-only first: under QEMU's disk throttle it also uses up the
       burst the throttle allows at the start, so the run that matters —
       with the checksum — sees the steady disk speed. */
    join(dir, "big4");
    t = now_us();
    if (stream(path, 0, &bytes, &sum) < 0) {
        put("streambench: no ");
        put(path);
        put("\n");
        return 1;
    }
    report("read only 8 MiB, cold         ", bytes, now_us() - t);

    join(dir, "big3");
    t = now_us();
    if (stream(path, 1, &bytes, &sum) < 0)
        return 1;
    report("read + checksum 8 MiB, cold   ", bytes, now_us() - t);
    return 0;
}
