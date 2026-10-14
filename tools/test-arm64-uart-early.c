/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Host test: arm64_uart_init_from_dtb picks s5l vs PL011 from DTB (step 15b).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../arch/aarch64/include/apple/s5l_uart.h"
#include "../arch/aarch64/include/apple/uart_early.h"
#include "../arch/aarch64/include/uart.h"

static uintptr_t s_base;
static int s_s5l;

void uart_set_base(uintptr_t phys)
{
    s_base = phys;
    s_s5l = 0;
}

void uart_use_apple_s5l(uintptr_t phys)
{
    s_base = phys;
    s_s5l = 1;
}

void uart_init(void)
{
}

static void *map_file(const char *path, size_t *len)
{
    int fd;
    struct stat st;
    void *m;

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
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        perror("mmap");
    else if (len)
        *len = (size_t)st.st_size;
    return m;
}

int main(int argc, char **argv)
{
    const char *m1 = argc > 1 ? argv[1] : "build/arm64/m1-min.dtb";
    const char *harness = argc > 2 ? argv[2] : "build/arm64/m1-qemu-harness.dtb";
    void *map;
    size_t sz;

    map = map_file(m1, &sz);
    if (map == MAP_FAILED)
        return 1;
    s_base = 0;
    s_s5l = 0;
    arm64_uart_init_from_dtb((uintptr_t)map);
    if (!s_s5l || s_base != (uintptr_t)ARM64_APPLE_M1_AIR_UART_PHYS) {
        fprintf(stderr, "FAIL: m1-min should bind apple,s5l-uart @ 0x%llx (got s5l=%d base=0x%lx)\n",
                (unsigned long long)ARM64_APPLE_M1_AIR_UART_PHYS, s_s5l,
                (unsigned long)s_base);
        munmap(map, sz);
        return 1;
    }
    printf("OK: m1-min -> s5l @ 0x%lx\n", (unsigned long)s_base);
    munmap(map, sz);

    map = map_file(harness, &sz);
    if (map == MAP_FAILED)
        return 1;
    s_base = 0;
    s_s5l = 0;
    arm64_uart_init_from_dtb((uintptr_t)map);
    if (s_s5l || s_base != 0x09000000u) {
        fprintf(stderr, "FAIL: harness should use PL011 @ 0x9000000 (s5l=%d base=0x%lx)\n",
                s_s5l, (unsigned long)s_base);
        munmap(map, sz);
        return 1;
    }
    printf("OK: harness -> PL011 @ 0x%lx\n", (unsigned long)s_base);
    munmap(map, sz);
    printf("UART early DTB pick OK\n");
    return 0;
}
