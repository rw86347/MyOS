/*
 * elf.c — load a static i386 ELF executable into user memory
 *
 * The loader trusts nothing in the file: every offset and size is checked
 * against the file size, and every address must be in user space. A broken
 * or hostile file must give -ENOEXEC, never crash the kernel.
 */
#include <elf.h>
#include <errno.h>
#include <memlayout.h>
#include <mm.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <vfs.h>

/* Map the pages covering [va, va+len), writable for now (we're about to
   copy into them; the kernel respects read-only pages, CR0.WP). A page may
   already be mapped by the previous segment: two segments can share the
   page where one ends and the next begins; then we keep it. */
static int map_segment(uint32_t va, uint32_t len)
{
    uint32_t first = va & ~0xFFFu;
    uint32_t last = (va + len - 1) & ~0xFFFu;

    for (uint32_t page = first; ; page += PAGE_SIZE) {
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

/* Does any PT_LOAD segment with flag `pf` touch this page? */
static int page_has(const struct elf32_phdr *ph, int n, uint32_t page, uint32_t pf)
{
    for (int i = 0; i < n; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & pf) && ph[i].p_memsz &&
            ph[i].p_vaddr < page + PAGE_SIZE && ph[i].p_vaddr + ph[i].p_memsz > page)
            return 1;
    return 0;
}

/* After loading: make the pages of read-only segments (code, constants)
   read-only, unless a writable segment shares the page. */
static void protect_segments(const struct elf32_phdr *ph, int n)
{
    for (int i = 0; i < n; i++) {
        if (ph[i].p_type != PT_LOAD || (ph[i].p_flags & PF_W) || !ph[i].p_memsz)
            continue;
        uint32_t last = (ph[i].p_vaddr + ph[i].p_memsz - 1) & ~0xFFFu;
        for (uint32_t page = ph[i].p_vaddr & ~0xFFFu; ; page += PAGE_SIZE) {
            if (!page_has(ph, n, page, PF_W))
                vmm_protect(page, PTE_USER);
            if (page == last)
                break;
        }
    }
}

/* Tell the address space where the program's code and data are, page by
   page with the right permissions (a page shared by two segments gets
   both). mm_add merges neighbours with equal flags, so this ends up as a
   few areas. Returns the first page after the last segment. */
static uint32_t add_areas(const struct elf32_phdr *ph, int n)
{
    uint32_t top = 0;
    for (int i = 0; i < n; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz)
            continue;
        uint32_t last = (ph[i].p_vaddr + ph[i].p_memsz - 1) & ~0xFFFu;
        for (uint32_t page = ph[i].p_vaddr & ~0xFFFu; ; page += PAGE_SIZE) {
            uint32_t flags = VM_READ;
            if (page_has(ph, n, page, PF_W))
                flags |= VM_WRITE;
            if (page_has(ph, n, page, PF_X))
                flags |= VM_EXEC;
            /* (A page two segments share is added once; the repeat is
               refused as an overlap, harmlessly.) */
            mm_add(mm_current(), page, page + PAGE_SIZE, flags);
            if (page == last)
                break;
        }
        if (last + PAGE_SIZE > top)
            top = last + PAGE_SIZE;
    }
    return top;
}

int elf_load(const void *image, uint32_t size, uint32_t dyn_base, struct elf_info *info)
{
    const uint8_t *file = image;
    const struct elf32_ehdr *eh = image;

    /* 1. Is this an ELF file for us? */
    if (size < sizeof *eh ||
        memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 ||
        eh->e_ident[4] != ELFCLASS32 ||         /* 32-bit */
        eh->e_ident[5] != ELFDATA2LSB ||        /* little endian */
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        eh->e_machine != EM_386 ||
        eh->e_phentsize != sizeof(struct elf32_phdr) ||
        eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHDRS ||
        eh->e_phoff > size ||
        eh->e_phnum * sizeof(struct elf32_phdr) > size - eh->e_phoff)
        return -ENOEXEC;

    /* Our own copy of the program headers, so we can shift addresses. */
    struct elf32_phdr ph[ELF_MAX_PHDRS];
    memcpy(ph, file + eh->e_phoff, eh->e_phnum * sizeof ph[0]);

    /* Does it name a dynamic linker (PT_INTERP)? The kernel loads that
       too (usermode.c); here we just check the name is a proper string. */
    info->interp = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_INTERP)
            continue;
        if (ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset ||
            ph[i].p_filesz < 2 || ph[i].p_filesz > PATH_MAX ||
            file[ph[i].p_offset + ph[i].p_filesz - 1] != '\0')
            return -ENOEXEC;
        info->interp = (const char *)file + ph[i].p_offset;
    }

    /* A position-independent file (ET_DYN: a PIE program, or the dynamic
       linker itself) can run at any address: we pick one and shift every
       segment by the difference; its own start-up code fixes its
       pointers. */
    uint32_t bias = 0;
    if (eh->e_type == ET_DYN) {
        uint32_t lowest = 0xFFFFFFFF, highest = 0;
        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD)
                continue;
            if (ph[i].p_vaddr < lowest)
                lowest = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > highest)
                highest = ph[i].p_vaddr + ph[i].p_memsz;
        }
        lowest &= ~0xFFFu;
        if (highest <= lowest)
            return -ENOEXEC;
        if (dyn_base == ELF_BASE_ANY) {
            dyn_base = mm_find_gap(mm_current(), ((highest - lowest) + 0xFFF) & ~0xFFFu);
            if (!dyn_base)
                return -ENOMEM;
        }
        bias = dyn_base - lowest;
        for (int i = 0; i < eh->e_phnum; i++)
            ph[i].p_vaddr += bias;
    }
    info->base = bias;
    info->entry = eh->e_entry + bias;
    info->phnum = eh->e_phnum;
    info->phdr = 0;

    /* 2. Load every PT_LOAD segment. */
    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf32_phdr *p = &ph[i];

        if (p->p_type == PT_PHDR)
            info->phdr = p->p_vaddr;
        if (p->p_type != PT_LOAD || p->p_memsz == 0)
            continue;

        uint32_t end = p->p_vaddr + p->p_memsz;
        if (p->p_filesz > p->p_memsz ||
            p->p_offset > size || p->p_filesz > size - p->p_offset ||
            p->p_vaddr < PAGE_SIZE ||           /* keep NULL unmapped */
            end < p->p_vaddr || end > KERNEL_VBASE)
            return -ENOEXEC;

        if (map_segment(p->p_vaddr, p->p_memsz) < 0)
            return -ENOMEM;

        /* The pages are mapped in the current address space, so we copy
           straight to the program's addresses. */
        memcpy((void *)p->p_vaddr, file + p->p_offset, p->p_filesz);
        memset((void *)(p->p_vaddr + p->p_filesz), 0, p->p_memsz - p->p_filesz);

        /* No PT_PHDR? The headers are usually inside the first segment,
           which starts at file offset 0. The program (musl) needs to find
           them in memory to set up thread-local storage. */
        if (!info->phdr && p->p_offset == 0 && eh->e_phoff < p->p_filesz)
            info->phdr = p->p_vaddr + eh->e_phoff;
    }

    if (info->entry < PAGE_SIZE || info->entry >= KERNEL_VBASE ||
        !vmm_translate(info->entry))
        return -ENOEXEC;

    protect_segments(ph, eh->e_phnum);
    info->brk_start = add_areas(ph, eh->e_phnum);
    return 0;
}
