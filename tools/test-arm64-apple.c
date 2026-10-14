/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Host test: Apple vs QEMU DTB platform probe (step 15a).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../arch/aarch64/include/apple/machine.h"
#include "../arch/aarch64/include/apple/s5l_uart.h"
#include "../arch/aarch64/include/fdt.h"

static int test_dtb(const char *path, enum arm64_platform want, const char *label_need)
{
    int fd;
    struct stat st;
    void *map;
    enum arm64_platform got;
    char label[128];

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    got = arm64_platform_from_dtb((uintptr_t)map);
    arm64_platform_label((uintptr_t)map, label, sizeof label);
    if (got != want) {
        fprintf(stderr, "FAIL: %s platform %d want %d (label=%s)\n", path, got, want,
                label);
        munmap(map, (size_t)st.st_size);
        return 1;
    }
    if (label_need && !strstr(label, label_need)) {
        fprintf(stderr, "FAIL: %s label %s missing %s\n", path, label, label_need);
        munmap(map, (size_t)st.st_size);
        return 1;
    }
    printf("OK: %s -> %s\n", path, label);
    munmap(map, (size_t)st.st_size);
    return 0;
}

static void *map_dtb(const char *path, size_t *size_out)
{
    int fd;
    struct stat st;
    void *map;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return MAP_FAILED;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return MAP_FAILED;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap");
        return MAP_FAILED;
    }
    if (size_out)
        *size_out = (size_t)st.st_size;
    return map;
}

static int test_uart_reg(const char *path, const char *compat, uint64_t want)
{
    void *map;
    size_t sz;
    uint64_t base;

    map = map_dtb(path, &sz);
    if (map == MAP_FAILED)
        return 1;
    if (fdt_mmio_by_compatible((uintptr_t)map, compat, &base, NULL) < 0) {
        fprintf(stderr, "FAIL: %s reg missing in %s\n", compat, path);
        munmap(map, sz);
        return 1;
    }
    if (base != want) {
        fprintf(stderr, "FAIL: %s base 0x%llx want 0x%llx in %s\n", compat,
                (unsigned long long)base, (unsigned long long)want, path);
        munmap(map, sz);
        return 1;
    }
    printf("OK: %s %s @ 0x%llx\n", path, compat, (unsigned long long)base);
    munmap(map, sz);
    return 0;
}

static int test_ram(const char *path, uint64_t want_base, uint64_t want_size)
{
    void *map;
    size_t sz;
    uint64_t base, size;

    map = map_dtb(path, &sz);
    if (map == MAP_FAILED)
        return 1;
    if (fdt_first_ram((uintptr_t)map, &base, &size) < 0) {
        fprintf(stderr, "FAIL: /memory reg not found in %s\n", path);
        munmap(map, sz);
        return 1;
    }
    if (base != want_base || size != want_size) {
        fprintf(stderr,
                "FAIL: %s RAM 0x%llx + 0x%llx (want 0x%llx + 0x%llx)\n", path,
                (unsigned long long)base, (unsigned long long)size,
                (unsigned long long)want_base, (unsigned long long)want_size);
        munmap(map, sz);
        return 1;
    }
    printf("OK: %s RAM 0x%llx + 0x%llx\n", path, (unsigned long long)base,
           (unsigned long long)size);
    munmap(map, sz);
    return 0;
}

int main(int argc, char **argv)
{
    const char *virt = argc > 1 ? argv[1] : "build/arm64/virt-embed.dtb";
    const char *apple = argc > 2 ? argv[2] : "build/arm64/m1-min.dtb";
    const char *harness = argc > 3 ? argv[3] : "build/arm64/m1-qemu-harness.dtb";

    if (test_dtb(virt, ARM64_PLAT_QEMU_VIRT, "QEMU") != 0)
        return 1;
    if (test_dtb(apple, ARM64_PLAT_APPLE, "MacBook Air") != 0)
        return 1;
    if (test_ram(apple, 0x80000000u, 0x40000000u) != 0)
        return 1;
    if (test_uart_reg(apple, "apple,s5l-uart", ARM64_APPLE_M1_AIR_UART_PHYS) != 0)
        return 1;
    if (test_uart_reg(harness, "apple,uart", 0x09000000u) != 0)
        return 1;
    printf("Apple Silicon DTB probe OK\n");
    return 0;
}
