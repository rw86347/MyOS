/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_ELF64_LOAD_H
#define ARCH_AARCH64_ELF64_LOAD_H

#include <stddef.h>
#include <stdint.h>

struct arm64_elf_loaded {
    void *entry;
    void *user_sp;
    uint64_t brk;
    uint64_t at_base;
    uint64_t at_entry;
    uint64_t at_phdr;
    uint16_t at_phent;
    uint16_t at_phnum;
};

int arm64_elf_load_exec(const void *image, size_t size,
                        const char *const *argv, const char *const *envp,
                        const void *interp, size_t interp_len,
                        const char *execfn, struct arm64_elf_loaded *out);

int arm64_elf_is_image(const void *image, size_t size);
int arm64_elf_interp(const void *image, size_t size, char *out, size_t outsz);

#endif
