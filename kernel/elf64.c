/*
 * elf64.c — load ELF64 programs (x86_64 only)
 *
 * elf_map maps a program file page by page on demand (the way elf.c does
 * for i386); elf_load checks and copies an image already in memory (the
 * self-tests' hand-made images).
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
            base = mm_find_gap(mm_current(), page_up(hi - lo));
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

/* Does any PT_LOAD segment with flag `pf` touch this page? */
static int page_has(const struct elf64_phdr *ph, int n, uint64_t page, uint32_t pf)
{
    for (int i = 0; i < n; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & pf) && ph[i].p_memsz &&
            ph[i].p_vaddr < page + PAGE_SIZE && ph[i].p_vaddr + ph[i].p_memsz > page)
            return 1;
    return 0;
}

/* The furthest file offset that still holds real bytes of any segment
   covering this page. Past that, the page is BSS: zeros. */
static uint32_t page_limit(const struct elf64_phdr *ph, int n, uint64_t page)
{
    uint64_t limit = 0;
    for (int i = 0; i < n; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz &&
            ph[i].p_vaddr < page + PAGE_SIZE && ph[i].p_vaddr + ph[i].p_memsz > page &&
            ph[i].p_offset + ph[i].p_filesz > limit)
            limit = ph[i].p_offset + ph[i].p_filesz;
    return (uint32_t)limit;
}

/* Did an earlier PT_LOAD segment (one before `i`) already map this page?
   Two segments can share the page where one ends and the next begins. */
static int page_taken(const struct elf64_phdr *ph, int i, uint64_t page)
{
    for (int j = 0; j < i; j++)
        if (ph[j].p_type == PT_LOAD && ph[j].p_memsz &&
            (ph[j].p_vaddr & ~0xFFFull) <= page && ph[j].p_vaddr + ph[j].p_memsz > page)
            return 1;
    return 0;
}

/*
 * Map a program file into the current address space, as the 32-bit loader
 * (elf.c) does: only the headers are read now. Each segment becomes a
 * file-backed area, and a page is read from the file (through the page
 * cache) the first time the program touches it; past a segment's file
 * bytes it is zeros (BSS). A 220 MB browser starts by reading the few
 * pages it runs, not all of it, and needs no 220 MB buffer.
 *
 * Pages next to each other with the same rights and end-of-data go in as
 * one area (one mm_add_file call per run, not per page).
 */
int elf_map(struct inode *ino, user_addr_t dyn_base, struct elf_info *info,
            char *interp, uint32_t interp_cap)
{
    struct elf64_ehdr eh;
    if (!ino || ino->size < sizeof eh || file_copy_out(ino, 0, &eh, sizeof eh) < 0)
        return -ENOEXEC;
    if (memcmp(eh.e_ident, "\177ELF", 4) || eh.e_ident[4] != ELFCLASS64 ||
        eh.e_ident[5] != ELFDATA2LSB || eh.e_machine != EM_X86_64 ||
        (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) ||
        eh.e_phentsize != sizeof(struct elf64_phdr) ||
        eh.e_phnum == 0 || eh.e_phnum > ELF_MAX_PHDRS ||
        eh.e_phoff > ino->size ||
        (uint64_t)eh.e_phnum * sizeof(struct elf64_phdr) > ino->size - eh.e_phoff)
        return -ENOEXEC;
    int n = eh.e_phnum;
    struct elf64_phdr ph[ELF_MAX_PHDRS];
    if (file_copy_out(ino, (uint32_t)eh.e_phoff, ph, n * sizeof ph[0]) < 0)
        return -EIO;

    /* 1. Check every segment against the file; the span they cover. */
    info->interp = NULL;
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < n; i++) {
        if (ph[i].p_type == PT_INTERP) {
            if (ph[i].p_offset > ino->size || ph[i].p_filesz > ino->size - ph[i].p_offset ||
                ph[i].p_filesz < 2 || ph[i].p_filesz > interp_cap || ph[i].p_filesz > PATH_MAX)
                return -ENOEXEC;
            if (file_copy_out(ino, (uint32_t)ph[i].p_offset, interp, (uint32_t)ph[i].p_filesz) < 0)
                return -EIO;
            if (interp[ph[i].p_filesz - 1] != '\0')
                return -ENOEXEC;
            info->interp = interp;
        }
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz)
            continue;
        /* A page holds file bytes at the same offset within the page as in
           memory, or the file can't be mapped page by page (Linux refuses
           such files too). */
        if (ph[i].p_filesz > ph[i].p_memsz ||
            ph[i].p_offset > ino->size || ph[i].p_filesz > ino->size - ph[i].p_offset ||
            ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr ||
            (ph[i].p_vaddr & 0xFFF) != (ph[i].p_offset & 0xFFF))
            return -ENOEXEC;
        if ((ph[i].p_vaddr & ~0xFFFull) < lo)
            lo = ph[i].p_vaddr & ~0xFFFull;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi)
            hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (hi == 0)
        return -ENOEXEC;

    /* 2. Where it goes: a position-independent file (ET_DYN) is shifted. */
    uint64_t bias = 0;
    if (eh.e_type == ET_DYN) {
        uint64_t base = dyn_base;
        if (dyn_base == ELF_BASE_ANY) {
            base = mm_find_gap(mm_current(), page_up(hi - lo));
            if (!base)
                return -ENOMEM;
        }
        bias = base - lo;
    }
    if (lo + bias < USER_ADDR_MIN || hi + bias > USER_ADDR_END || hi + bias < lo + bias)
        return -ENOEXEC;
    for (int i = 0; i < n; i++)
        ph[i].p_vaddr += bias;

    /* 3. The areas, page runs of equal rights and end-of-data. */
    info->phdr = 0;
    for (int i = 0; i < n; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->p_type == PT_PHDR)
            info->phdr = p->p_vaddr;
        if (p->p_type != PT_LOAD || !p->p_memsz)
            continue;
        uint64_t map_start = p->p_vaddr & ~0xFFFull;
        uint64_t map_end = page_up(p->p_vaddr + p->p_memsz);
        uint64_t file_off0 = p->p_offset - (p->p_vaddr - map_start);
        uint64_t run = 0;               /* the run's first page (0: none open) */
        uint32_t run_flags = 0, run_limit = 0;
        for (uint64_t page = map_start; page <= map_end; page += PAGE_SIZE) {
            uint32_t flags = 0, limit = 0;
            int take = page < map_end && !page_taken(ph, i, page);
            if (take) {
                flags = VM_READ | (page_has(ph, n, page, PF_W) ? VM_WRITE : 0) |
                        (page_has(ph, n, page, PF_X) ? VM_EXEC : 0);
                limit = page_limit(ph, n, page);
            }
            if (run && (!take || flags != run_flags || limit != run_limit)) {
                int err = mm_add_file(mm_current(), run, page, run_flags, ino,
                                      (uint32_t)(file_off0 + (run - map_start)), run_limit);
                if (err)
                    return err;
                run = 0;
            }
            if (take && !run) {
                run = page;
                run_flags = flags;
                run_limit = limit;
            }
        }
        /* No PT_PHDR: the headers are usually in the first segment (offset
           0); musl needs them in memory (AT_PHDR) for thread-local storage. */
        if (!info->phdr && p->p_offset == 0 && eh.e_phoff < p->p_filesz)
            info->phdr = p->p_vaddr + eh.e_phoff;
    }
    info->entry = eh.e_entry + bias;
    info->base = bias;
    info->brk_start = page_up(hi + bias);
    info->phnum = eh.e_phnum;
    if (info->entry < USER_ADDR_MIN || info->entry >= USER_ADDR_END)
        return -ENOEXEC;
    /* The rest of the file waits until the program touches it. */
    if (mm_fault(info->entry, 0) < 0 || !vmm_translate(info->entry))
        return -ENOEXEC;
    return 0;
}

#endif
