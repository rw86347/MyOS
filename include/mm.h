#ifndef MM_H
#define MM_H

#include <stdint.h>

/*
 * An address space: one process's view of memory (Linux: mm_struct).
 *
 * The page tables say which pages EXIST right now. The list of areas
 * (VMAs, "virtual memory areas") says which addresses are ALLOWED: the
 * program's code and data, its heap (brk), its stack, mmap'd memory. A page
 * fault inside an area is not an error — it's "demand paging": the page is
 * created right then, zeroed. So a 8 MiB stack or a big malloc costs
 * nothing until it's actually touched.
 */

#define VM_READ   1
#define VM_WRITE  2
#define VM_EXEC   4

#define MM_AREAS  32

/* Where things go in a program's memory:
     0x08048000   code and data (from the ELF file), then the heap (brk)
     0x40000000   mmap() areas, growing upward
     0xB7FFF000   the stack area: 8 MiB, the stack grows down from the top
     0xBFFFF000   the vsyscall page (arch/i386/sysenter.h)
     0xC0000000   the kernel */
#define USER_STACK_TOP    0xBFFFF000u
#define USER_STACK_MAX    (8u << 20)
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_MAX)
#define MMAP_BASE         0x40000000u

struct inode;

struct vma {
    uint32_t start, end;                /* [start, end), page aligned */
    uint32_t flags;                     /* VM_READ / VM_WRITE / VM_EXEC */
    struct inode *file;                 /* mmap'd file (NULL: zero-filled) */
    uint32_t file_off;                  /* file offset of `start` */
    /* Exclusive file offset of real bytes. ~0 means "through the end of
       the file" (mmap). An ELF segment sets this to where its bytes stop,
       so the BSS past p_filesz is zeros even when the file continues. */
    uint32_t file_end;
};

struct mm {
    uint32_t pd;                        /* page directory (physical) */
    struct vma area[MM_AREAS];          /* sorted by start, no overlaps */
    int nareas;
    uint32_t brk_start, brk;            /* the heap: [brk_start, brk) */
};

void mm_init(void);                     /* after vmm_init() */

struct mm *mm_create(void);             /* empty user space */
struct mm *mm_fork(struct mm *mm);      /* copy-on-write copy of mm (current) */
void mm_destroy(struct mm *mm);         /* must not be the current one */

struct mm *mm_current(void);
struct mm *mm_kernel(void);             /* no user space at all */
void mm_switch(struct mm *mm);          /* load its page directory */

/* Add an area (demand-zero pages). 0, or -errno (overlap, too many). */
int mm_add(struct mm *mm, uint32_t start, uint32_t end, uint32_t flags);

/* Add an area whose pages come from a file (copied in on first touch).
   file_end is the exclusive file offset of real bytes, or ~0u for the
   whole file. */
int mm_add_file(struct mm *mm, uint32_t start, uint32_t end, uint32_t flags,
                struct inode *file, uint32_t file_off, uint32_t file_end);

/* The lowest free stretch of len bytes from MMAP_BASE up, or 0. */
uint32_t mm_find_gap(struct mm *mm, uint32_t len);

/* Remove [start, end) from the areas and unmap its pages. */
int mm_remove(struct mm *mm, uint32_t start, uint32_t end);

/* A page fault at va (CPU error code err) in the current address space:
   0 if it was fixed (demand paging, copy-on-write), -1 if it's a real error. */
int mm_fault(uint32_t va, uint32_t err);

/* The brk and mmap system calls, on the current address space. */
uint32_t mm_brk(uint32_t new_brk);
int32_t  mm_mmap(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags,
                 struct inode *file, uint32_t off);
int32_t  mm_munmap(uint32_t addr, uint32_t len);
int32_t  mm_mprotect(uint32_t addr, uint32_t len, uint32_t prot);

#endif
