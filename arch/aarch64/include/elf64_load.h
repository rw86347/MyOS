/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_ELF64_LOAD_H
#define ARCH_AARCH64_ELF64_LOAD_H

#include <stddef.h>
#include <stdint.h>

struct arm64_elf_loaded {
    void *entry;
    void *user_sp;
};

int arm64_elf_load_exec(const void *image, size_t size, struct arm64_elf_loaded *out);

#endif
