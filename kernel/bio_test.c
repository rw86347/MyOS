/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * bio_test.c — requests that don't wait (make test, step 9d-3)
 *
 * Two reads are submitted to the boot disk one after the other. Submitting
 * must not wait for the disk (both are still busy right after), each must
 * call its done function exactly once, and the bytes must be the same as
 * an ordinary read of the same sectors. A request past the end of the disk
 * is refused at once.
 */
#include <blockdev.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <selftest.h>
#include <string.h>

static int dones;

static void count_done(struct bio *b)
{
    (void)b;
    dones++;
}

void bio_selftest(void)
{
    struct blockdev *dev = blockdev_get(0);
    uint32_t want = pmm_alloc(), got = pmm_alloc();
    if (!dev || !want || !got) {
        check(0, "bio: a disk and two frames");
        return;
    }
    check(blockdev_read(dev, 0, 16, P2V(want)) == 0, "bio: plain read");

    /* Sectors 0-3 into the first half of the frame, 4-7 into the second. */
    struct bio_vec v1 = { got, 2048 }, v2 = { got + 2048, 2048 };
    struct bio a = { .lba = 0, .v = &v1, .nv = 1, .done = count_done };
    struct bio b = { .lba = 4, .v = &v2, .nv = 1, .done = count_done };
    memset(P2V(got), 0xEE, 4096);
    dones = 0;
    int ok = blockdev_submit(dev, &a) == 0 && blockdev_submit(dev, &b) == 0;
    check(ok && a.status == BIO_BUSY && b.status == BIO_BUSY,
          "bio: submit returns before the disk is done");
    int sa = bio_wait(&a), sb = bio_wait(&b);
    check(sa == 0 && sb == 0 && dones == 2, "bio: both finish, done called once each");
    check(memcmp(P2V(got), P2V(want), 4096) == 0, "bio: same bytes as a plain read");
    if (sa == 0 && sb == 0 && memcmp(P2V(got), P2V(want), 4096) == 0)
        kprintf("bio: two requests queued without waiting, same bytes\n");

    struct bio bad = { .lba = dev->sectors, .v = &v1, .nv = 1 };
    check(blockdev_submit(dev, &bad) < 0, "bio: past the end is refused");

    pmm_free(want);
    pmm_free(got);
}
