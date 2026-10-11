/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_CPIO_H
#define ARCH_AARCH64_CPIO_H

#include <stddef.h>
#include <stdint.h>

#define ARM64_S_IFMT   0170000
#define ARM64_S_IFDIR  0040000
#define ARM64_S_IFREG  0100000
#define ARM64_S_IFLNK  0120000

int arm64_cpio_find(const void *archive, size_t size, const char *name,
                    const void **out, size_t *out_len);
int arm64_cpio_file_count(const void *archive, size_t size);

/* Lookup a member or a synthesized directory. mode is the newc mode. */
int arm64_cpio_lookup(const void *archive, size_t size, const char *name,
                      uint32_t *mode, const void **data, size_t *len);

/* Write linux_dirent64 records for dir ("/" or "bin"). Returns bytes used. */
int arm64_cpio_getdents(const void *archive, size_t size, const char *dir,
                        void *buf, size_t buflen);

#endif
