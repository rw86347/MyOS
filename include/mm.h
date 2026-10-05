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
/* Device memory (Linux: VM_PFNMAP): the area maps a fixed physical range
   (a framebuffer), not RAM pages. Its pages are shared by everyone who maps
   them — across fork too — and never freed. VM_WC: write-combining. */
#define VM_PFNMAP 0x100
#define VM_WC     0x200
#define VM_MAYWRITE 0x400               /* device opened read-write: mprotect may add write */
/* Really shared (MAP_SHARED of a file in RAM, MAP_SHARED|MAP_ANONYMOUS,
   shmat): `file`'s own pages are mapped, the same frames in every process
   that maps them, across fork too (PTE_MAPSHARED). A write is seen by all
   of them and by read(). VM_MAYWRITE: the file was opened read-write. */
#define VM_SHARED 0x800
#define VM_SHM    0x1000                /* VM_SHARED, attached by shmat (kernel/shm.c) */

#define MM_AREAS_FIRST  32             /* areas a new address space has room for */
#define MM_AREAS_MAX  1024              /* it doubles up to this (GTK programs: ~400) */

/* Where things go in a program's memory:
     0x08048000   code and data (from the ELF file), then the heap (brk)
     0x40000000   mmap() areas, growing upward
     0xB7FFF000   the stack area: 8 MiB, the stack grows down from the top
     0xBFFFF000   the vsyscall page (arch/i386/sysenter.h)
     0xC0000000   the kernel */
#include <user.h>      /* USER_STACK_TOP, MMAP_BASE, ... for this CPU */

struct inode;

struct vma {
    user_addr_t start, end;             /* [start, end), page aligned */
    uint32_t flags;                     /* VM_READ / VM_WRITE / VM_EXEC */
    struct inode *file;                 /* mmap'd file (NULL: zero-filled) */
    uint32_t file_off;                  /* file offset of `start` */
    /* Exclusive file offset of real bytes. ~0 means "through the end of
       the file" (mmap). An ELF segment sets this to where its bytes stop,
       so the BSS past p_filesz is zeros even when the file continues. */
    uint32_t file_end;
    /* VM_PFNMAP: the physical address of file offset 0 (file_end is then
       the device's length). */
    uint32_t phys;
};

struct mm {
    uint32_t pd;                        /* page directory (physical) */
    struct vma *area;                   /* sorted by start, no overlaps */
    int nareas, cap;                    /* in use, room for */
    user_addr_t brk_start, brk;         /* the heap: [brk_start, brk) */
};

void mm_init(void);                     /* after vmm_init() */

struct mm *mm_create(void);             /* empty user space */
struct mm *mm_fork(struct mm *mm);      /* copy-on-write copy of mm (current) */
void mm_destroy(struct mm *mm);         /* must not be the current one */

struct mm *mm_current(void);
struct mm *mm_kernel(void);             /* no user space at all */
void mm_switch(struct mm *mm);          /* load its page directory */

/* Add an area (demand-zero pages). 0, or -errno (overlap, too many). */
int mm_add(struct mm *mm, user_addr_t start, user_addr_t end, uint32_t flags);

/* Add an area whose pages come from a file (copied in on first touch).
   file_end is the exclusive file offset of real bytes, or ~0u for the
   whole file. */
int mm_add_file(struct mm *mm, user_addr_t start, user_addr_t end, uint32_t flags,
                struct inode *file, uint32_t file_off, uint32_t file_end);

/* The lowest free stretch of len bytes from MMAP_BASE up, or 0. */
user_addr_t mm_find_gap(struct mm *mm, uint32_t len);

/* Remove [start, end) from the areas and unmap its pages. */
int mm_remove(struct mm *mm, user_addr_t start, user_addr_t end);

/* A page fault at va (CPU error code err) in the current address space:
   0 if it was fixed (demand paging, copy-on-write), -1 if it's a real error. */
int mm_fault(user_addr_t va, uint32_t err);

/* The brk and mmap system calls, on the current address space. fmode: the
   file's open() flags (its O_ACCMODE decides what MAP_SHARED may do). */
user_addr_t mm_brk(user_addr_t new_brk);
intptr_t mm_mmap(user_addr_t addr, uint32_t len, uint32_t prot, uint32_t flags,
                 struct inode *file, uint32_t off, uint32_t fmode);
int32_t  mm_munmap(user_addr_t addr, uint32_t len);
intptr_t mm_mremap(user_addr_t old, uint32_t old_len, uint32_t new_len,
                   uint32_t flags, user_addr_t new_addr);

/* mmap of a device file whose ops have .mmap (/dev/fb0). The area is
   always shared: writes reach the device, as with MAP_SHARED. */
struct file;
intptr_t mm_mmap_device(user_addr_t addr, uint32_t len, uint32_t prot, uint32_t flags,
                        struct file *f, uint32_t off);

/* For a device's .mmap: make vma map physical [phys, phys + size),
   starting at the vma's file offset (Linux: vm_iomap_memory). 0, or
   -EINVAL if the vma reaches past size. */
int vm_iomap_memory(struct vma *vma, uint32_t phys, uint32_t size);
int32_t  mm_mprotect(user_addr_t addr, uint32_t len, uint32_t prot);

/* shmdt: remove the shared-memory attach that starts at addr — every area
   of that segment in [addr, addr + its size), in the current address
   space. Returns the segment, held (the caller vfs_puts it), or NULL if no
   attach starts at addr. */
struct inode *mm_shm_detach(user_addr_t addr);

#endif
