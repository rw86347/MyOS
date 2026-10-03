#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/*
 * ELF (Executable and Linkable Format) — the file format of Linux programs.
 * Only what the loader needs. Field names follow the ELF spec / <elf.h>.
 *
 *   +-------------------+  offset 0
 *   | ELF header        |  "\x7fELF", 32/64-bit, CPU, entry address,
 *   |                   |  where the program header table is
 *   +-------------------+  e_phoff
 *   | program headers   |  one per segment: "copy file bytes [p_offset,
 *   | (e_phnum of them) |   +p_filesz) to address p_vaddr, p_memsz long
 *   +-------------------+   (rest zeroed), with permissions p_flags"
 *   | code, data, ...   |
 *   +-------------------+
 *
 * (Section headers — .text, .data, symbols — are for linkers and debuggers;
 * the loader ignores them.)
 */

struct elf32_ehdr {
    uint8_t  e_ident[16];       /* magic, class, byte order, version, ... */
    uint16_t e_type;            /* ET_EXEC = fixed-address executable */
    uint16_t e_machine;         /* EM_386 */
    uint32_t e_version;
    uint32_t e_entry;           /* address of the first instruction */
    uint32_t e_phoff;           /* file offset of the program headers */
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;       /* size of one program header */
    uint16_t e_phnum;           /* how many */
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type;            /* PT_LOAD = load this into memory */
    uint32_t p_offset;          /* where the bytes are in the file */
    uint32_t p_vaddr;           /* where they go in memory */
    uint32_t p_paddr;
    uint32_t p_filesz;          /* bytes in the file */
    uint32_t p_memsz;           /* bytes in memory (>= filesz; rest = zero) */
    uint32_t p_flags;           /* PF_R / PF_W / PF_X */
    uint32_t p_align;
} __attribute__((packed));

#define ELFCLASS32   1
#define ELFDATA2LSB  1          /* little endian */
#define ET_EXEC      2
#define EM_386       3

#define ET_DYN       3          /* position independent (here: static PIE) */

#define PT_LOAD      1
#define PT_INTERP    3          /* "load this dynamic linker first" */
#define PT_PHDR      6

#define ELF_MAX_PHDRS 16
#define ELF_DYN_BASE  0x00400000u   /* where a position-independent program goes */

#define PF_X         1
#define PF_W         2
#define PF_R         4

/* Result of loading an ELF file into the current address space. */
struct elf_info {
    uint32_t entry;             /* where to start */
    uint32_t phdr;              /* address of the program headers in memory */
    uint32_t phnum;
    uint32_t brk_start;         /* first page after the segments: the heap */
    uint32_t base;              /* how far it was shifted (position independent) */
    const char *interp;         /* PT_INTERP: the dynamic linker it wants, or NULL */
};

/* Check the ELF image (size bytes) and load its segments into user memory.
   A position-independent file (ET_DYN) goes to dyn_base — or, with
   ELF_BASE_ANY, to the first free place in the mmap area.
   Returns 0, or a negative errno (-ENOEXEC: not a usable i386 executable). */
#define ELF_BASE_ANY 0
int elf_load(const void *image, uint32_t size, uint32_t dyn_base, struct elf_info *info);

struct inode;

/* Map `ino` into the current address space and fault in only the entry
   page. The rest is filled on first touch (and the next page is read
   ahead). `interp`, when the file names a dynamic linker, receives that
   path; info->interp then points at `interp`. */
int elf_map(struct inode *ino, uint32_t dyn_base, struct elf_info *info,
            char *interp, uint32_t interp_cap);

#endif
