/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Host unit test for arch/aarch64/cpio.c
 *   make ARCH=arm64 test-arm64-cpio
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../arch/aarch64/include/cpio.h"

static uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

static void put(uint8_t **pp, const char *name, const uint8_t *data, uint32_t dlen)
{
    char hdr[111];
    uint32_t namesize = (uint32_t)strlen(name) + 1;
    uint32_t npad, dpad;

    snprintf(hdr, sizeof hdr,
             "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
             1u, 0100644u, 0u, 0u, 1u, 0u, dlen, 0u, 0u, 0u, 0u, namesize, 0u);
    memcpy(*pp, hdr, 110);
    *pp += 110;
    memcpy(*pp, name, namesize);
    *pp += namesize;
    npad = align4(110 + namesize) - 110 - namesize;
    memset(*pp, 0, npad);
    *pp += npad;
    if (dlen)
        memcpy(*pp, data, dlen);
    *pp += dlen;
    dpad = align4(dlen) - dlen;
    memset(*pp, 0, dpad);
    *pp += dpad;
}

int main(void)
{
    uint8_t buf[512];
    uint8_t *p = buf;
    const uint8_t payload[] = "INITOK";
    const void *found;
    size_t len;
    int n;

    const uint8_t hello[] = "cat from initramfs\n";

    memset(buf, 0, sizeof buf);
    put(&p, "init", payload, sizeof payload - 1);
    put(&p, "hello.txt", hello, sizeof hello - 1);
    put(&p, "TRAILER!!!", NULL, 0);

    n = arm64_cpio_file_count(buf, (size_t)(p - buf));
    if (n != 2) {
        fprintf(stderr, "count %d want 2\n", n);
        return 1;
    }
    if (arm64_cpio_find(buf, (size_t)(p - buf), "/init", &found, &len) < 0 ||
        len != sizeof payload - 1 || memcmp(found, payload, len) != 0) {
        fprintf(stderr, "find /init failed\n");
        return 1;
    }
    if (arm64_cpio_find(buf, (size_t)(p - buf), "/hello.txt", &found, &len) < 0 ||
        len != sizeof hello - 1 || memcmp(found, hello, len) != 0) {
        fprintf(stderr, "find /hello.txt failed\n");
        return 1;
    }
    printf("CPIO unit test OK: found /init and /hello.txt\n");
    return 0;
}
