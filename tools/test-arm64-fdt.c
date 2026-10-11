/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Host unit test for arch/aarch64/fdt.c (run: make ARCH=arm64 test-arm64-fdt)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../arch/aarch64/include/fdt.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/arm64/virt-embed.dtb";
    int fd;
    struct stat st;
    void *map;
    uint64_t base, size;
    int r;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return 1;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (fdt_check_header((uintptr_t)map) < 0) {
        fprintf(stderr, "FAIL: bad FDT header in %s\n", path);
        return 1;
    }
    r = fdt_first_ram((uintptr_t)map, &base, &size);
    if (r < 0) {
        fprintf(stderr, "FAIL: /memory reg not found in %s\n", path);
        munmap(map, (size_t)st.st_size);
        return 1;
    }
    if (base != 0x40000000UL || size != 0x8000000UL) {
        fprintf(stderr, "FAIL: unexpected RAM 0x%llx + 0x%llx (want 0x40000000 + 0x8000000)\n",
                (unsigned long long)base, (unsigned long long)size);
        munmap(map, (size_t)st.st_size);
        return 1;
    }
    printf("FDT unit test OK: memory@40000000 size 128 MiB\n");
    munmap(map, (size_t)st.st_size);
    return 0;
}
