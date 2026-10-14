/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Host test: M1 Air UART MMIO indices + console reg from m1-min.dtb (step 15b).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../arch/aarch64/include/apple/mmio_map.h"
#include "../arch/aarch64/include/apple/s5l_uart.h"
#include "../arch/aarch64/include/apple/uart_early.h"
#include "../arch/aarch64/include/uart.h"

void uart_set_base(uintptr_t phys)
{
    (void)phys;
}

void uart_use_apple_s5l(uintptr_t phys)
{
    (void)phys;
}

int main(int argc, char **argv)
{
    const char *m1 = argc > 1 ? argv[1] : "build/arm64/m1-min.dtb";
    int fd;
    struct stat st;
    void *map;
    uint64_t base, size;
    unsigned l1, l2;

    {
        uint64_t block = arm64_mmio_block_align(ARM64_APPLE_M1_AIR_UART_PHYS);

        arm64_mmio_block_indices(block, &l1, &l2);
        if (block != ARM64_APPLE_M1_AIR_UART_PHYS) {
            fprintf(stderr, "FAIL: 2 MiB align for M1 UART (got 0x%llx)\n",
                    (unsigned long long)block);
            return 1;
        }
        if (l1 != 8u || l2 != 425u) {
            fprintf(stderr, "FAIL: M1 UART indices l1=%u l2=%u (want 8, 425)\n", l1, l2);
            return 1;
        }
        printf("OK: M1 UART MMIO l1=%u l2=%u block=0x%llx\n", l1, l2,
               (unsigned long long)block);
    }

    fd = open(m1, O_RDONLY);
    if (fd < 0) {
        perror(m1);
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
    if (arm64_early_console_mmio((uintptr_t)map, &base, &size) != 0) {
        fprintf(stderr, "FAIL: early console MMIO missing in %s\n", m1);
        munmap(map, (size_t)st.st_size);
        return 1;
    }
    if (base != ARM64_APPLE_M1_AIR_UART_PHYS || size != 0x1000u) {
        fprintf(stderr, "FAIL: console 0x%llx + 0x%llx\n", (unsigned long long)base,
                (unsigned long long)size);
        munmap(map, (size_t)st.st_size);
        return 1;
    }
    printf("OK: %s early console 0x%llx\n", m1, (unsigned long long)base);
    munmap(map, (size_t)st.st_size);
    printf("Apple MMIO map plan OK\n");
    return 0;
}
