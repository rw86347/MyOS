/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * cpio.c — newc walker for ARM64 bring-up (step 14b)
 *
 * Each member is a 110-byte ASCII hex header, then the name, then data.
 * Directories that only exist as path prefixes are synthesized.
 */
#include "cpio.h"

#include <stdint.h>
#include <string.h>

#define CPIO_HDR  110u

static int hex8(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    for (int i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;

        if (c >= '0' && c <= '9')
            d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (uint32_t)(c - 'A' + 10);
        else
            return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

static uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

static void skip_slashes(const char **p)
{
    const char *s = *p;

    while (s[0] == '/' || (s[0] == '.' && s[1] == '/'))
        s += s[0] == '/' ? 1 : 2;
    *p = s;
}

static int name_eq(const char *want, const char *have)
{
    skip_slashes(&want);
    skip_slashes(&have);
    while (*want && *have && *want == *have) {
        want++;
        have++;
    }
    return *want == '\0' && *have == '\0';
}

static int is_trailer(const char *nm, uint32_t namesize)
{
    if (namesize < 11)
        return 0;
    for (int i = 0; i < 11; i++) {
        if (nm[i] != "TRAILER!!!"[i])
            return 0;
    }
    return 1;
}

static int walk(const uint8_t *p, size_t size,
                int (*fn)(const char *name, uint32_t mode, const uint8_t *data,
                          uint32_t dlen, void *ctx),
                void *ctx)
{
    size_t off = 0;

    while (off + CPIO_HDR <= size) {
        const char *hdr = (const char *)(p + off);
        uint32_t namesize, filesize, mode, name_pad, rec;
        const char *nm;
        const uint8_t *data;

        if (hdr[0] != '0' || hdr[1] != '7' || hdr[2] != '0' || hdr[3] != '7' ||
            hdr[4] != '0' || hdr[5] != '1')
            return -1;
        if (hex8(hdr + 14, &mode) < 0 || hex8(hdr + 54, &filesize) < 0 ||
            hex8(hdr + 94, &namesize) < 0)
            return -1;
        if (namesize == 0 || off + CPIO_HDR + namesize > size)
            return -1;
        nm = (const char *)(p + off + CPIO_HDR);
        if (nm[namesize - 1] != '\0')
            return -1;
        name_pad = align4(CPIO_HDR + namesize);
        rec = name_pad + align4(filesize);
        if (off + rec > size)
            return -1;
        if (is_trailer(nm, namesize))
            return 0;
        data = p + off + name_pad;
        if (fn && fn(nm, mode, data, filesize, ctx) < 0)
            return 0;
        off += rec;
    }
    return -1;
}

struct find_ctx {
    const char *want;
    const void *data;
    size_t len;
    uint32_t mode;
    int found;
};

static int find_cb(const char *name, uint32_t mode, const uint8_t *data,
                   uint32_t dlen, void *ctx)
{
    struct find_ctx *c = ctx;

    if (!name_eq(c->want, name))
        return 0;
    c->data = data;
    c->len = dlen;
    c->mode = mode;
    c->found = 1;
    return -1;
}

static int count_cb(const char *name, uint32_t mode, const uint8_t *data,
                    uint32_t dlen, void *ctx)
{
    int *n = ctx;

    (void)name;
    (void)mode;
    (void)data;
    (void)dlen;
    (*n)++;
    return 0;
}

struct dir_ctx {
    const char *dir;
    int match;
};

static int prefix_cb(const char *name, uint32_t mode, const uint8_t *data,
                     uint32_t dlen, void *ctx)
{
    struct dir_ctx *c = ctx;
    const char *n = name;
    const char *d = c->dir;
    size_t dl;

    (void)mode;
    (void)data;
    (void)dlen;
    skip_slashes(&n);
    skip_slashes(&d);
    dl = 0;
    while (d[dl])
        dl++;
    if (!dl) {
        c->match = 1;
        return -1;
    }
    if (strncmp(n, d, dl) == 0 && n[dl] == '/') {
        c->match = 1;
        return -1;
    }
    return 0;
}

int arm64_cpio_find(const void *archive, size_t size, const char *name,
                    const void **out, size_t *out_len)
{
    uint32_t mode;

    return arm64_cpio_lookup(archive, size, name, &mode, out, out_len);
}

int arm64_cpio_file_count(const void *archive, size_t size)
{
    int n = 0;

    if (!archive || walk(archive, size, count_cb, &n) < 0)
        return -1;
    return n;
}

int arm64_cpio_lookup(const void *archive, size_t size, const char *name,
                      uint32_t *mode, const void **data, size_t *len)
{
    struct find_ctx c = { name, 0, 0, 0, 0 };
    struct dir_ctx d;
    const char *n = name;

    if (!archive || !name || !mode || !data || !len)
        return -1;
    if (walk(archive, size, find_cb, &c) < 0 && !c.found)
        return -1;
    if (c.found) {
        *mode = c.mode;
        *data = c.data;
        *len = c.len;
        return 0;
    }
    skip_slashes(&n);
    if (!n[0]) {
        *mode = ARM64_S_IFDIR | 0755;
        *data = 0;
        *len = 0;
        return 0;
    }
    d.dir = n;
    d.match = 0;
    walk(archive, size, prefix_cb, &d);
    if (!d.match)
        return -1;
    *mode = ARM64_S_IFDIR | 0755;
    *data = 0;
    *len = 0;
    return 0;
}

struct dent_ctx {
    const char *dir;
    char names[64][64];
    uint8_t types[64];
    int n;
};

static int dent_cb(const char *name, uint32_t mode, const uint8_t *data,
                   uint32_t dlen, void *ctx)
{
    struct dent_ctx *c = ctx;
    const char *path = name;
    const char *dir = c->dir;
    char comp[64];
    size_t dl = 0, i = 0;
    int k, is_dir;
    uint8_t typ;

    (void)data;
    (void)dlen;
    skip_slashes(&path);
    skip_slashes(&dir);
    while (dir[dl])
        dl++;
    if (dl) {
        if (strncmp(path, dir, dl) != 0 || path[dl] != '/')
            return 0;
        path += dl + 1;
    }
    if (!path[0])
        return 0;
    while (path[i] && path[i] != '/' && i + 1 < sizeof comp) {
        comp[i] = path[i];
        i++;
    }
    comp[i] = 0;
    is_dir = path[i] == '/';
    if ((mode & ARM64_S_IFMT) == ARM64_S_IFLNK)
        typ = 10; /* DT_LNK */
    else if (is_dir || (mode & ARM64_S_IFMT) == ARM64_S_IFDIR)
        typ = 4;
    else
        typ = 8;
    for (k = 0; k < c->n; k++) {
        if (strcmp(c->names[k], comp) == 0)
            return 0;
    }
    if (c->n >= 64)
        return 0;
    memcpy(c->names[c->n], comp, i + 1);
    c->types[c->n] = typ;
    c->n++;
    return 0;
}

int arm64_cpio_getdents(const void *archive, size_t size, const char *dir,
                        void *buf, size_t buflen)
{
    struct dent_ctx c;
    size_t used = 0;
    int i;

    memset(&c, 0, sizeof c);
    c.dir = dir ? dir : "";
    if (!archive || !buf)
        return -1;
    walk(archive, size, dent_cb, &c);
    for (i = 0; i < c.n; i++) {
        size_t nlen = strlen(c.names[i]) + 1;
        size_t rec = (19 + nlen + 7) & ~7ull;
        uint8_t *p;
        uint64_t ino = (uint64_t)(i + 2);
        int64_t off = i + 1;
        uint16_t reclen;

        if (used + rec > buflen)
            break;
        p = (uint8_t *)buf + used;
        memset(p, 0, rec);
        memcpy(p, &ino, 8);
        memcpy(p + 8, &off, 8);
        reclen = (uint16_t)rec;
        memcpy(p + 16, &reclen, 2);
        p[18] = c.types[i];
        memcpy(p + 19, c.names[i], nlen);
        used += rec;
    }
    return (int)used;
}
