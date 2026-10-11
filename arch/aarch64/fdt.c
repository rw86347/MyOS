/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fdt.c — minimal device-tree reader (no libfdt)
 */
#include "fdt.h"

#include <stddef.h>
#include <stdint.h>

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

#define FDT_MAX_DEPTH 16

static uint32_t be32(const void *p)
{
    const uint8_t *b = p;

    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static const uint8_t *fdt_align4(const uint8_t *p)
{
    return (const uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
}

static uint64_t be64(const void *p)
{
    uint32_t hi;
    uint32_t lo;

    /* FDT props are 4-byte aligned only; avoid an unaligned ldr from -O2. */
    __builtin_memcpy(&hi, p, sizeof(hi));
    __builtin_memcpy(&lo, (const char *)p + 4, sizeof(lo));
    hi = __builtin_bswap32(hi);
    lo = __builtin_bswap32(lo);
    return ((uint64_t)hi << 32) | lo;
}

static int name_is_memory(const char *name, size_t namelen)
{
    if (namelen < 6)
        return 0;
    return name[0] == 'm' && name[1] == 'e' && name[2] == 'm' && name[3] == 'o' &&
           name[4] == 'r' && name[5] == 'y';
}

static int prop_is(const char *key, uint32_t nameoff, uint32_t str_size, const char *lit)
{
    size_t i = 0;
    size_t litlen = 0;

    if (nameoff >= str_size)
        return 0;
    while (lit[litlen])
        litlen++;
    if (nameoff + litlen >= str_size)
        return 0;
    while (i < litlen) {
        if (key[i] != lit[i])
            return 0;
        i++;
    }
    return key[i] == '\0';
}

int fdt_check_header(uintptr_t dtb_phys)
{
    const struct fdt_header *h = (const struct fdt_header *)dtb_phys;

    if (be32(&h->magic) != FDT_MAGIC)
        return -1;
    if (be32(&h->version) < 17)
        return -1;
    return 0;
}

static int parse_reg(const uint8_t *val, uint32_t len, int addr_cells, int size_cells,
                     uint64_t *base_out, uint64_t *size_out)
{
    if (addr_cells == 2 && size_cells == 2 && len >= 16) {
        *base_out = be64(val);
        *size_out = be64(val + 8);
        return 0;
    }
    if (addr_cells == 2 && size_cells == 1 && len >= 12) {
        *base_out = be64(val);
        *size_out = be32(val + 8);
        return 0;
    }
    if (addr_cells == 1 && size_cells == 1 && len >= 8) {
        *base_out = be32(val);
        *size_out = be32(val + 4);
        return 0;
    }
    return -1;
}

int fdt_first_ram(uintptr_t dtb_phys, uint64_t *base_out, uint64_t *size_out)
{
    const struct fdt_header *h = (const struct fdt_header *)dtb_phys;
    const uint8_t *p;
    const uint8_t *struct_end;
    const uint8_t *blob_end;
    const char *strings;
    uint32_t str_size;
    int depth = 0;
    int addr_cells = 2;
    int size_cells = 2;
    int memory_depth = 0; /* depth of the innermost open "memory*" node, else 0 */

    if (fdt_check_header(dtb_phys) < 0)
        return -1;

    blob_end = (const uint8_t *)dtb_phys + be32(&h->totalsize);
    str_size = be32(&h->size_dt_strings);
    strings = (const char *)dtb_phys + be32(&h->off_dt_strings);
    p = (const uint8_t *)dtb_phys + be32(&h->off_dt_struct);
    struct_end = (const uint8_t *)dtb_phys + be32(&h->off_dt_struct) + be32(&h->size_dt_struct);
    if (p >= struct_end || struct_end > blob_end)
        return -1;
    if (be32(&h->off_dt_strings) + str_size > be32(&h->totalsize))
        return -1;

    while (p + 4 <= struct_end) {
        uint32_t token = be32(p);

        p += 4;
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            size_t namelen = 0;

            if (depth >= FDT_MAX_DEPTH)
                return -1;
            while (p + namelen < struct_end && name[namelen])
                namelen++;
            if (p + namelen >= struct_end)
                return -1;
            p += namelen + 1;
            p = fdt_align4(p);
            depth++;
            if (name_is_memory(name, namelen))
                memory_depth = depth;
            break;
        }
        case FDT_END_NODE:
            if (depth == memory_depth)
                memory_depth = 0;
            if (depth > 0)
                depth--;
            break;
        case FDT_PROP: {
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4);
            const char *key;
            const uint8_t *val;

            if (p + 8 > struct_end || nameoff >= str_size)
                return -1;
            key = strings + nameoff;
            val = p + 8;
            if (p + 8 + len > struct_end)
                return -1;
            p += 8 + len;
            p = fdt_align4(p);

            if (depth == 1) {
                if (prop_is(key, nameoff, str_size, "#address-cells") && len == 4)
                    addr_cells = (int)be32(val);
                if (prop_is(key, nameoff, str_size, "#size-cells") && len == 4)
                    size_cells = (int)be32(val);
            }
            if (depth == memory_depth && prop_is(key, nameoff, str_size, "reg")) {
                if (parse_reg(val, len, addr_cells, size_cells, base_out, size_out) == 0)
                    return 0;
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return -1;
        default:
            break;
        }
    }
    return -1;
}
