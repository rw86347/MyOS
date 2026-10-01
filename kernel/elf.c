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
#include <paging.h>
#include <pmm.h>
#include <string.h>

/* Map the pages covering [va, va+len) for a user segment. A page may
   already be mapped by the previous segment (two segments can share the
   page where one ends and the next begins); then we keep it, and make it
   writable if this segment needs that. */
static int map_segment(uint32_t va, uint32_t len, int writable)
{
    uint32_t flags = PTE_USER | (writable ? PTE_WRITABLE : 0);
    uint32_t first = va & ~0xFFFu;
    uint32_t last = (va + len - 1) & ~0xFFFu;

    for (uint32_t page = first; ; page += PAGE_SIZE) {
        uint32_t pa = vmm_translate(page);
        if (pa) {
            if (writable) {             /* re-map with the write bit */
                pa = vmm_unmap(page);
                vmm_map(page, pa, flags);
            }
        } else {
            pa = pmm_alloc();
            if (!pa)
                return -1;
            memset(P2V(pa), 0, PAGE_SIZE);
            if (vmm_map(page, pa, flags) < 0) {
                pmm_free(pa);
                return -1;
            }
        }
        if (page == last)
            return 0;
    }
}

int elf_load(const void *image, uint32_t size, struct elf_info *info)
{
    const uint8_t *file = image;
    const struct elf32_ehdr *eh = image;

    /* 1. Is this an ELF file for us? */
    if (size < sizeof *eh ||
        memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 ||
        eh->e_ident[4] != ELFCLASS32 ||         /* 32-bit */
        eh->e_ident[5] != ELFDATA2LSB ||        /* little endian */
        eh->e_type != ET_EXEC ||                /* static, fixed address */
        eh->e_machine != EM_386 ||
        eh->e_phentsize != sizeof(struct elf32_phdr) ||
        eh->e_phnum == 0 ||
        eh->e_phoff > size ||
        eh->e_phnum * sizeof(struct elf32_phdr) > size - eh->e_phoff)
        return -ENOEXEC;

    const struct elf32_phdr *ph = (const void *)(file + eh->e_phoff);
    info->entry = eh->e_entry;
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

        if (map_segment(p->p_vaddr, p->p_memsz, p->p_flags & PF_W) < 0)
            return -ENOMEM;

        /* The kernel can write even read-only user pages (CR0.WP is off),
           so we copy straight to the program's addresses. */
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
    return 0;
}
