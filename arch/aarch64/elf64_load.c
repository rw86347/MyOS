/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * elf64_load.c — load static ET_EXEC / ET_DYN (PIE) aarch64 musl images (bring-up)
 *
 * Identity-mapped RAM: copy PT_LOAD to p_vaddr, zero BSS. No relocation yet except
 * ET_DYN bias when load address != linked base (not needed for our musl-elf.ld ET_EXEC).
 */
#include "elf64_load.h"
#include "uaccess.h"

#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>

#define EM_AARCH64 183
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN  3
#define PT_LOAD 1

#define ARM64_USER_STACK_TOP  0x40480000ULL

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

static void sync_cache_range(uint64_t va, uint64_t len)
{
    uint64_t end = va + len;

    for (uint64_t a = va & ~63ull; a < end; a += 64)
        __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish");
    for (uint64_t a = va & ~63ull; a < end; a += 64)
        __asm__ volatile("ic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb sy");
}

static int user_vaddr_ok(uint64_t va, uint64_t len)
{
    if (len == 0)
        return 0;
    if (va < ARM64_USER_MIN)
        return -1;
    if (va + len < va)
        return -1;
    if (va + len > ARM64_USER_MAX)
        return -1;
    return 0;
}

static void *build_user_stack(const char *argv0)
{
    uint64_t *sp = (uint64_t *)(uintptr_t)ARM64_USER_STACK_TOP;
    size_t namelen = strlen(argv0) + 1;
    char *str = (char *)(uintptr_t)(ARM64_USER_STACK_TOP - 256);

    if (namelen > 200)
        namelen = 200;
    memset((void *)(uintptr_t)(ARM64_USER_STACK_TOP - PAGE_SIZE), 0, PAGE_SIZE);
    memcpy(str, argv0, namelen);

    sp = (uint64_t *)((uintptr_t)sp & ~0xfull);
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uint64_t)(uintptr_t)str;
    *--sp = 1;
    if ((uintptr_t)sp & 8)
        *--sp = 0;
    return sp;
}

int arm64_elf_load_exec(const void *image, size_t size, struct arm64_elf_loaded *out)
{
    const uint8_t *file = image;
    const struct elf64_ehdr *eh = image;
    if (size < sizeof *eh || memcmp(eh->e_ident, "\177ELF", 4) != 0 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_machine != EM_AARCH64 ||
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        eh->e_phentsize < sizeof(struct elf64_phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return -1;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(file + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD || !ph->p_memsz)
            continue;
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset ||
            ph->p_filesz > ph->p_memsz)
            return -1;
        if (user_vaddr_ok(ph->p_vaddr, ph->p_memsz) < 0) {
            kprintf("  ELF: segment 0x%llx len 0x%llx outside user range\n",
                    (unsigned long long)ph->p_vaddr, (unsigned long long)ph->p_memsz);
            return -1;
        }
        memcpy((void *)(uintptr_t)ph->p_vaddr, file + ph->p_offset, (size_t)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(ph->p_vaddr + ph->p_filesz), 0,
                   (size_t)(ph->p_memsz - ph->p_filesz));
        sync_cache_range(ph->p_vaddr, ph->p_memsz);
    }

    if (user_vaddr_ok(eh->e_entry, 1) < 0)
        return -1;

    out->entry = (void *)(uintptr_t)eh->e_entry;
    out->user_sp = build_user_stack("hello");
    return 0;
}
