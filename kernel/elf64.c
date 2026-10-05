/*
 * elf64.c — load static ELF64 executables (x86_64 only)
 */
#ifdef ARCH_X86_64

#include <elf.h>
#include <kheap.h>
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
        if (!vmm_translate(page)) {
            uint32_t pa = pmm_alloc();
            if (!pa)
                return -1;
            memset(P2V(pa), 0, PAGE_SIZE);
            if (vmm_map(page, pa, PTE_USER | PTE_WRITABLE) < 0) {
                pmm_free(pa);
                return -1;
            }
        }
        if (page == last)
            return 0;
    }
}

#define ET_EXEC    2
#define ET_DYN     3
#define PT_INTERP  3
#define PT_PHDR    6

static uint64_t page_up(uint64_t x) { return (x + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1); }

/*
 * Load a 64-bit ELF program into the current address space. A position-
 * independent one (ET_DYN: Alpine's programs, static or not, and the dynamic
 * linker) is shifted to dyn_base, or with ELF_BASE_ANY to the first free gap
 * of the mmap area; a fixed-address one (ET_EXEC: Alpine's busybox-static,
 * at 0x400000) is loaded where it was linked. Every segment must land in user
 * space, [USER_ADDR_MIN, USER_ADDR_END).
 */
int elf_load(const void *image, uint32_t size, user_addr_t dyn_base, struct elf_info *info)
{
    const uint8_t *file = image;
    if (size < sizeof(struct elf64_ehdr))
        return -ENOEXEC;
    const struct elf64_ehdr *eh = image;
    if (memcmp(eh->e_ident, "\177ELF", 4) || eh->e_ident[4] != ELFCLASS64
        || eh->e_ident[5] != ELFDATA2LSB || eh->e_machine != EM_X86_64
        || (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        || eh->e_phentsize < sizeof(struct elf64_phdr)
        || eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return -ENOEXEC;
#define PH(i) ((const struct elf64_phdr *)(file + eh->e_phoff + (uint64_t)(i) * eh->e_phentsize))

    /* 1. The span of the loadable segments, to place the whole program. */
    uint64_t lo = ~0ull, hi = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = PH(i);
        if (ph->p_type != PT_LOAD || !ph->p_memsz)
            continue;
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr || ph->p_filesz > ph->p_memsz ||
            ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
            return -ENOEXEC;
        if ((ph->p_vaddr & ~0xFFFull) < lo)
            lo = ph->p_vaddr & ~0xFFFull;
        if (ph->p_vaddr + ph->p_memsz > hi)
            hi = ph->p_vaddr + ph->p_memsz;
    }
    if (hi == 0)
        return -ENOEXEC;
    uint64_t bias = 0;
    if (eh->e_type == ET_DYN) {
        uint64_t base = dyn_base;
        if (dyn_base == ELF_BASE_ANY) {
            base = mm_find_gap(mm_current(), (uint32_t)page_up(hi - lo));
            if (!base)
                return -ENOMEM;
        }
        bias = base - lo;
    }
    if (lo + bias < USER_ADDR_MIN || hi + bias > USER_ADDR_END || hi + bias < lo + bias)
        return -ENOEXEC;

    /* 2. Load each segment: pages made on the spot, bytes copied, BSS zeroed. */
    info->phdr = 0;
    info->interp = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = PH(i);
        if (ph->p_type == PT_PHDR)
            info->phdr = ph->p_vaddr + bias;
        if (ph->p_type == PT_INTERP && ph->p_filesz > 1 && ph->p_offset < size &&
            ph->p_filesz <= size - ph->p_offset && file[ph->p_offset + ph->p_filesz - 1] == '\0')
            info->interp = (const char *)file + ph->p_offset;
        if (ph->p_type != PT_LOAD || !ph->p_memsz)
            continue;
        uint64_t va = ph->p_vaddr + bias;
        if (map_seg(va, ph->p_memsz) < 0)
            return -ENOMEM;
        memcpy((void *)(uintptr_t)va, file + ph->p_offset, (size_t)ph->p_filesz);
        memset((void *)(uintptr_t)(va + ph->p_filesz), 0, (size_t)(ph->p_memsz - ph->p_filesz));
        uint32_t flags = VM_READ | ((ph->p_flags & PF_W) ? VM_WRITE : 0)
                       | ((ph->p_flags & PF_X) ? VM_EXEC : 0);
        mm_add(mm_current(), va & ~0xFFFull, page_up(va + ph->p_memsz), flags);
        /* No PT_PHDR: the headers are usually in the first segment (offset 0);
           musl needs them in memory (AT_PHDR) for thread-local storage. */
        if (!info->phdr && ph->p_offset == 0 && eh->e_phoff < ph->p_filesz)
            info->phdr = va + eh->e_phoff;
    }
#undef PH
    info->entry = eh->e_entry + bias;
    info->base = bias;
    info->brk_start = page_up(hi + bias);
    info->phnum = eh->e_phnum;
    return 0;
}

/* elf_map for a file in the tree: its bytes in one piece (in place from
   the initramfs, else copied), loaded; the interpreter path copied out. */
int elf_map(struct inode *ino, user_addr_t dyn_base, struct elf_info *info,
            char *interp, uint32_t interp_cap)
{
    if (ino->size < sizeof(struct elf64_ehdr))
        return -ENOEXEC;
    const uint8_t *bytes = ino->data;
    uint8_t *tmp = NULL;
    if (!bytes || ino->pages) {
        tmp = kmalloc(ino->size);
        if (!tmp)
            return -ENOMEM;
        if (file_copy_out(ino, 0, tmp, ino->size) < 0) {
            kfree(tmp);
            return -EIO;
        }
        bytes = tmp;
    }
    int err = elf_load(bytes, ino->size, dyn_base, info);
    if (!err && interp && interp_cap) {
        if (info->interp) {
            uint32_t n = 0;
            while (info->interp[n] && n + 1 < interp_cap) {
                interp[n] = info->interp[n];
                n++;
            }
            interp[n] = '\0';
            info->interp = interp;      /* (the file bytes may go away) */
        }
    }
    kfree(tmp);
    return err;
}

#endif
