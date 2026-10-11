/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * elf64_load.c — load static ET_EXEC / ET_DYN and a PT_INTERP (musl ld.so)
 */
#include "elf64_load.h"
#include "uaccess.h"

#include <kprintf.h>
#include <memlayout.h>
#include <string.h>

#define EM_AARCH64 183
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN  3
#define PT_LOAD 1
#define PT_INTERP 3

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_EXECFN  31

#define MAX_ARGV  16
#define MAX_ENV   8

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

int arm64_elf_is_image(const void *image, size_t size)
{
    const struct elf64_ehdr *eh = image;

    return size >= sizeof *eh && memcmp(eh->e_ident, "\177ELF", 4) == 0 &&
           eh->e_ident[4] == ELFCLASS64 && eh->e_ident[5] == ELFDATA2LSB &&
           eh->e_machine == EM_AARCH64 &&
           (eh->e_type == ET_EXEC || eh->e_type == ET_DYN);
}

int arm64_elf_interp(const void *image, size_t size, char *out, size_t outsz)
{
    const uint8_t *file = image;
    const struct elf64_ehdr *eh = image;

    if (!arm64_elf_is_image(image, size) || !out || !outsz)
        return -1;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(file + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        size_t n;

        if (ph->p_type != PT_INTERP)
            continue;
        if (ph->p_offset >= size || !ph->p_filesz)
            return -1;
        n = (size_t)ph->p_filesz;
        if (n >= outsz)
            n = outsz - 1;
        memcpy(out, file + ph->p_offset, n);
        out[n] = 0;
        return 0;
    }
    return -1;
}

static int load_segments(const uint8_t *file, size_t size, uint64_t bias,
                         uint64_t *brk_out)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)file;
    uint64_t brk = 0;

    if (eh->e_phentsize < sizeof(struct elf64_phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return -1;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(file + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        uint64_t va, end;

        if (ph->p_type != PT_LOAD || !ph->p_memsz)
            continue;
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset ||
            ph->p_filesz > ph->p_memsz)
            return -1;
        va = bias + ph->p_vaddr;
        if (arm64_user_range_ok((uintptr_t)va, (size_t)ph->p_memsz) < 0) {
            kprintf("  ELF: segment 0x%llx len 0x%llx outside user range\n",
                    (unsigned long long)va, (unsigned long long)ph->p_memsz);
            return -1;
        }
        memcpy((void *)(uintptr_t)va, file + ph->p_offset, (size_t)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(va + ph->p_filesz), 0,
                   (size_t)(ph->p_memsz - ph->p_filesz));
        sync_cache_range(va, ph->p_memsz);
        end = va + ph->p_memsz;
        if (end > brk)
            brk = end;
    }
    *brk_out = brk;
    return 0;
}

static void *build_user_stack(const char *const *argv, const char *const *envp,
                              const char *execfn, const struct arm64_elf_loaded *info)
{
    uint64_t top = ARM64_USER_STACK_TOP;
    uint8_t *rnd = (uint8_t *)(uintptr_t)(top - 16);
    char *str = (char *)(uintptr_t)(top - 512);
    const char *aptrs[MAX_ARGV];
    const char *eptrs[MAX_ENV];
    uint64_t *sp;
    int argc = 0, envc = 0;
    uint64_t words;
    const char *ef;

    memset((void *)(uintptr_t)(top - 0x10000ULL), 0, 0x10000ULL);
    for (int i = 0; i < 16; i++)
        rnd[i] = (uint8_t)(0xa5 ^ i);

    if (!argv) {
        static const char *def[] = { "/init", 0 };

        argv = def;
    }
    if (!envp) {
        static const char *defe[] = { "PATH=/bin:/usr/bin", "HOME=/", "TERM=linux", 0 };

        envp = defe;
    }
    ef = execfn ? execfn : argv[0];
    for (; argv[argc] && argc < MAX_ARGV; argc++) {
        size_t n = strlen(argv[argc]) + 1;

        memcpy(str, argv[argc], n);
        aptrs[argc] = str;
        str += n;
    }
    for (; envp[envc] && envc < MAX_ENV; envc++) {
        size_t n = strlen(envp[envc]) + 1;

        memcpy(str, envp[envc], n);
        eptrs[envc] = str;
        str += n;
    }
    {
        size_t n = strlen(ef) + 1;

        memcpy(str, ef, n);
        execfn = str;
    }

    /* argc, argv..., NULL, envp..., NULL, auxv pairs (14) + NULL */
    words = 1ull + (uint64_t)argc + 1ull + (uint64_t)envc + 1ull + 30ull;
    if (words & 1ull)
        words++;
    sp = (uint64_t *)(((uintptr_t)(top - 512)) & ~15ull) - words;
    *sp++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        *sp++ = (uint64_t)(uintptr_t)aptrs[i];
    *sp++ = 0;
    for (int i = 0; i < envc; i++)
        *sp++ = (uint64_t)(uintptr_t)eptrs[i];
    *sp++ = 0;
    *sp++ = AT_PHDR;
    *sp++ = info->at_phdr;
    *sp++ = AT_PHENT;
    *sp++ = info->at_phent;
    *sp++ = AT_PHNUM;
    *sp++ = info->at_phnum;
    *sp++ = AT_PAGESZ;
    *sp++ = PAGE_SIZE;
    *sp++ = AT_BASE;
    *sp++ = info->at_base;
    *sp++ = AT_ENTRY;
    *sp++ = info->at_entry;
    *sp++ = AT_RANDOM;
    *sp++ = (uint64_t)(uintptr_t)rnd;
    *sp++ = AT_SECURE;
    *sp++ = 0;
    *sp++ = AT_UID;
    *sp++ = 0;
    *sp++ = AT_EUID;
    *sp++ = 0;
    *sp++ = AT_GID;
    *sp++ = 0;
    *sp++ = AT_EGID;
    *sp++ = 0;
    *sp++ = AT_EXECFN;
    *sp++ = (uint64_t)(uintptr_t)execfn;
    *sp++ = AT_NULL;
    *sp++ = 0;
    return (uint64_t *)(((uintptr_t)(top - 512)) & ~15ull) - words;
}

int arm64_elf_load_exec(const void *image, size_t size,
                        const char *const *argv, const char *const *envp,
                        const void *interp, size_t interp_len,
                        const char *execfn, struct arm64_elf_loaded *out)
{
    const uint8_t *file = image;
    const struct elf64_ehdr *eh = image;
    uint64_t bias = 0;
    uint64_t brk = 0;

    if (!arm64_elf_is_image(image, size))
        return -1;

    if (eh->e_type == ET_DYN)
        bias = ARM64_USER_IMAGE_VA;

    if (load_segments(file, size, bias, &brk) < 0)
        return -1;

    out->at_phdr = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(file + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;
        if (eh->e_phoff >= ph->p_offset &&
            eh->e_phoff < ph->p_offset + ph->p_filesz) {
            out->at_phdr = bias + ph->p_vaddr + (eh->e_phoff - ph->p_offset);
            break;
        }
    }
    if (!out->at_phdr)
        out->at_phdr = bias + eh->e_phoff;
    out->at_phent = eh->e_phentsize;
    out->at_phnum = eh->e_phnum;
    out->at_entry = bias + eh->e_entry;
    out->at_base = 0;
    out->brk = (brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    out->entry = (void *)(uintptr_t)out->at_entry;

    if (interp && interp_len) {
        const struct elf64_ehdr *ie = interp;
        uint64_t ibrk = 0;

        if (!arm64_elf_is_image(interp, interp_len))
            return -1;
        if (load_segments(interp, interp_len, ARM64_INTERP_VA, &ibrk) < 0)
            return -1;
        out->at_base = ARM64_INTERP_VA;
        out->entry = (void *)(uintptr_t)(ARM64_INTERP_VA + ie->e_entry);
        (void)ibrk;
    }

    if (arm64_user_range_ok((uintptr_t)out->entry, 1) < 0)
        return -1;
    out->user_sp = build_user_stack(argv, envp, execfn, out);
    return 0;
}
