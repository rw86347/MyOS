/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * initrd_test.c — did the whole initramfs arrive?
 *
 * cpio "newc" archives start with the magic "070701" and end with a file
 * named "TRAILER!!!". If both are where they should be, every 32 KiB chunk
 * stage 2 copied landed in the right place.
 */
#include <initrd.h>
#include <memlayout.h>
#include <pmm.h>
#include <selftest.h>
#include <string.h>

/* Is `needle` somewhere in the last `tail` bytes of the archive? */
static int ends_with(const char *needle, uint32_t tail)
{
    const uint8_t *data = initrd_data();
    uint32_t size = initrd_size(), n = strlen(needle);
    uint32_t from = size > tail ? size - tail : 0;
    for (uint32_t i = from; i + n <= size; i++)
        if (memcmp(data + i, needle, n) == 0)
            return 1;
    return 0;
}

void initrd_selftest(void)
{
    const uint8_t *data = initrd_data();
    uint32_t size = initrd_size();

    check(data && size >= 6 && memcmp(data, "070701", 6) == 0,
          "initramfs: starts with cpio magic");
    check(ends_with("TRAILER!!!", 1024), "initramfs: ends with cpio trailer");

    /* The page allocator must never hand out the initramfs' memory. It gives
       out the lowest free frame, and the initramfs sits right after the
       kernel — so if it weren't reserved, this frame would be inside it. */
    uint32_t phys = V2P(data);
    uint32_t f = pmm_alloc();
    check(f < phys || f >= phys + size, "initramfs: its memory is reserved");
    pmm_free(f);
}
