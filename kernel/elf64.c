/*
 * elf64.c — load static ELF64 executables (x86_64 only)
 */
#ifdef ARCH_X86_64

#include <elf.h>
#include <errno.h>
#include <memlayout.h>
#include <mm.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <user.h>
#include <vfs.h>

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

#define EM_X86_64 62
#define ELFCLASS64 2

static int map_seg(uint64_t va, uint64_t len)
{
    uint64_t first = va & ~0xFFFull;
    uint64_t last = (va + len - 1) & ~0xFFFull;
    for (uint64_t page = first; ; page += PAGE_SIZE) {
        if (!vmm_translate((uint32_t)page)) {
            uint32_t pa = pmm_alloc();
            if (!pa)
                return -1;
            memset(P2V(pa), 0, PAGE_SIZE);
            if (vmm_map((uint32_t)page, pa, PTE_USER | PTE_WRITABLE) < 0) {
                pmm_free(pa);
                return -1;
            }
        }
        if (page == last)
            return 0;
    }
}

int elf_load(const void *image, uint32_t size, uint32_t dyn_base, struct elf_info *info)
{
    (void)dyn_base;
    const uint8_t *file = image;
    if (size < sizeof(struct elf64_ehdr))
        return -ENOEXEC;
    const struct elf64_ehdr *eh = image;
    if (memcmp(eh->e_ident, "\177ELF", 4) || eh->e_ident[4] != ELFCLASS64
        || eh->e_ident[5] != ELFDATA2LSB || eh->e_machine != EM_X86_64)
        return -ENOEXEC;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return -ENOEXEC;

    uint64_t brk = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)
            (file + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || !ph->p_memsz)
            continue;
        if (ph->p_vaddr > USER_ADDR_MAX || ph->p_vaddr + ph->p_memsz > USER_ADDR_MAX)
            return -ENOEXEC;
        if (ph->p_offset + ph->p_filesz > size)
            return -ENOEXEC;
        if (map_seg(ph->p_vaddr, ph->p_memsz) < 0)
            return -ENOMEM;
        memcpy((void *)(uintptr_t)ph->p_vaddr, file + ph->p_offset, (size_t)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(uintptr_t)(ph->p_vaddr + ph->p_filesz), 0,
                   (size_t)(ph->p_memsz - ph->p_filesz));
        uint64_t end = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~0xFFFull;
        if (end > brk)
            brk = end;
        uint32_t flags = VM_READ | ((ph->p_flags & PF_W) ? VM_WRITE : 0)
                       | ((ph->p_flags & PF_X) ? VM_EXEC : 0);
        mm_add(mm_current(), (uint32_t)ph->p_vaddr, (uint32_t)end, flags);
    }
    info->entry = (uint32_t)eh->e_entry;
    info->brk_start = (uint32_t)brk;
    info->base = 0;
    info->interp = NULL;
    info->phdr = 0;
    info->phnum = 0;
    return 0;
}

int elf_map(struct inode *ino, uint32_t dyn_base, struct elf_info *info,
            char *interp, uint32_t interp_cap)
{
    (void)dyn_base;
    (void)interp;
    (void)interp_cap;
    if (!ino->data || ino->size < 64)
        return -ENOEXEC;
    return elf_load(ino->data, ino->size, 0, info);
}

#endif
