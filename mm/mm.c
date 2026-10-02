/*
 * mm.c — address spaces: areas, demand paging, brk, mmap (see mm.h)
 *
 * The CPU-specific work (page directories, copy-on-write bits) is in
 * arch/<cpu>/paging.c; this file decides what is allowed where.
 *
 * Speed: an address space has at most MM_AREAS areas, kept sorted, so every
 * lookup is a short scan of a small array that fits in a few cache lines.
 * Programs typically have 4-6 areas (code, data, heap, stack, a few mmaps).
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <mm.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <vfs.h>

static struct mm kernel_mm;
static struct mm *current_mm = &kernel_mm;

#define PAGE_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_DOWN(x) ((x) & ~(PAGE_SIZE - 1))

void mm_init(void)
{
    kernel_mm.pd = vmm_pd_current();    /* the boot page directory */
}

struct mm *mm_current(void) { return current_mm; }
struct mm *mm_kernel(void)  { return &kernel_mm; }

void mm_switch(struct mm *mm)
{
    if (mm != current_mm) {
        vmm_pd_switch(mm->pd);
        current_mm = mm;
    }
}

struct mm *mm_create(void)
{
    struct mm *mm = kmalloc(sizeof *mm);
    if (!mm)
        return NULL;
    memset(mm, 0, sizeof *mm);
    mm->pd = vmm_pd_create();
    if (!mm->pd) {
        kfree(mm);
        return NULL;
    }
    return mm;
}

struct mm *mm_fork(struct mm *mm)
{
    struct mm *child = kmalloc(sizeof *child);
    if (!child)
        return NULL;
    memcpy(child, mm, sizeof *child);   /* same areas, same brk */
    child->pd = vmm_pd_fork();          /* same pages, shared copy-on-write */
    if (!child->pd) {
        kfree(child);
        return NULL;
    }
    return child;
}

void mm_destroy(struct mm *mm)
{
    vmm_pd_destroy(mm->pd);
    kfree(mm);
}

/* The area containing addr, or NULL. */
static struct vma *find(struct mm *mm, uint32_t addr)
{
    for (int i = 0; i < mm->nareas; i++) {
        if (addr < mm->area[i].start)
            return NULL;                /* sorted: no later area can match */
        if (addr < mm->area[i].end)
            return &mm->area[i];
    }
    return NULL;
}

static int overlaps(struct mm *mm, uint32_t start, uint32_t end)
{
    for (int i = 0; i < mm->nareas; i++)
        if (mm->area[i].start < end && start < mm->area[i].end)
            return 1;
    return 0;
}

int mm_add(struct mm *mm, uint32_t start, uint32_t end, uint32_t flags)
{
    return mm_add_file(mm, start, end, flags, NULL, 0);
}

int mm_add_file(struct mm *mm, uint32_t start, uint32_t end, uint32_t flags,
                struct inode *file, uint32_t file_off)
{
    if (start >= end || end > KERNEL_VBASE || overlaps(mm, start, end))
        return -EINVAL;

    /* Grow a neighbour instead of adding an area, if it touches and has the
       same flags (the heap growing with brk does this all the time).
       File areas stay separate. */
    for (int i = 0; i < mm->nareas && !file; i++) {
        struct vma *v = &mm->area[i];
        if (v->flags != flags || v->file)
            continue;
        if (v->end == start && (i + 1 == mm->nareas || mm->area[i + 1].start >= end)) {
            v->end = end;
            return 0;
        }
        if (v->start == end) {
            v->start = start;
            return 0;
        }
    }

    if (mm->nareas == MM_AREAS)
        return -ENOMEM;
    int i = mm->nareas;
    while (i > 0 && mm->area[i - 1].start > start) {    /* keep them sorted */
        mm->area[i] = mm->area[i - 1];
        i--;
    }
    mm->area[i] = (struct vma){ start, end, flags, file, file_off };
    mm->nareas++;
    return 0;
}

int mm_remove(struct mm *mm, uint32_t start, uint32_t end)
{
    for (int i = 0; i < mm->nareas; i++) {
        struct vma *v = &mm->area[i];
        if (v->end <= start || end <= v->start)
            continue;
        if (start > v->start && end < v->end) {         /* a hole in the middle */
            if (mm->nareas == MM_AREAS)
                return -ENOMEM;
            memmove(&mm->area[i + 2], &mm->area[i + 1],
                    (mm->nareas - i - 1) * sizeof *v);
            mm->area[i + 1] = (struct vma){ end, v->end, v->flags, v->file,
                                            v->file_off + (end - v->start) };
            v->end = start;
            mm->nareas++;
            break;
        }
        if (start <= v->start && end >= v->end) {       /* all of it */
            memmove(v, v + 1, (mm->nareas - i - 1) * sizeof *v);
            mm->nareas--;
            i--;
        } else if (start <= v->start) {                 /* its beginning */
            v->file_off += end - v->start;
            v->start = end;
        } else {                                        /* its end */
            v->end = start;
        }
    }
    if (mm == current_mm)
        vmm_unmap_range(start, end);
    return 0;
}

int mm_fault(uint32_t va, uint32_t err)
{
    if (va >= KERNEL_VBASE)
        return -1;
    struct vma *v = find(current_mm, va);
    if (!v)
        return -1;                      /* nothing is allowed here: SIGSEGV */

    int write = err & 2;
    if (err & 1) {                      /* the page exists: a permission fault */
        if (write && (v->flags & VM_WRITE))
            return vmm_cow_fault(va);   /* copy-on-write after fork */
        return -1;
    }
    if (write && !(v->flags & VM_WRITE))
        return -1;

    /* Demand paging: the first touch of a page in an area creates it —
       zero-filled, or for a mapped file, a copy of that part of the file
       (straight from the initramfs). Pages never touched cost nothing. */
    uint32_t frame = pmm_alloc();
    if (!frame)
        return -1;                      /* out of memory: the program dies */
    uint32_t page = PAGE_DOWN(va), n = 0;
    if (v->file) {
        uint32_t off = v->file_off + (page - v->start);
        if (off < v->file->size)
            n = v->file->size - off < PAGE_SIZE ? v->file->size - off : PAGE_SIZE;
        memcpy(P2V(frame), v->file->data + off, n);
    }
    memset((uint8_t *)P2V(frame) + n, 0, PAGE_SIZE - n);
    uint32_t flags = PTE_USER | (v->flags & VM_WRITE ? PTE_WRITABLE : 0);
    if (vmm_map(page, frame, flags) < 0) {
        pmm_free(frame);
        return -1;
    }
    return 0;
}

/* --- brk ------------------------------------------------------------------ */

uint32_t mm_brk(uint32_t new_brk)
{
    struct mm *mm = current_mm;
    if (new_brk < mm->brk_start)
        return mm->brk;                 /* brk(0) etc.: just report it */

    uint32_t old_end = PAGE_UP(mm->brk), new_end = PAGE_UP(new_brk);
    if (new_end > old_end) {
        if (mm_add(mm, old_end, new_end, VM_READ | VM_WRITE) < 0)
            return mm->brk;             /* would hit another area: refuse */
    } else if (new_end < old_end) {
        mm_remove(mm, new_end, old_end);
    }
    mm->brk = new_brk;
    return new_brk;
}

/* --- mmap ----------------------------------------------------------------- */

#define PROT_WRITE    2
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* The lowest gap of len bytes at or above MMAP_BASE, below the stack. */
uint32_t mm_find_gap(struct mm *mm, uint32_t len)
{
    uint32_t addr = MMAP_BASE;
    for (int i = 0; i < mm->nareas; i++) {
        struct vma *v = &mm->area[i];
        if (v->end <= addr)
            continue;
        if (v->start >= addr + len)
            break;                      /* fits before this area */
        addr = v->end;
    }
    if (addr + len > USER_STACK_BOTTOM || addr + len < addr)
        return 0;
    return addr;
}

int32_t mm_mmap(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags,
                struct inode *file, uint32_t off)
{
    if (!(flags & MAP_ANONYMOUS)) {
        /* A file: only ones whose bytes are in memory (the initramfs).
           Mapped privately: writes change our copy, never the file. */
        if (!file || !S_ISREG(file->mode) || !file->data)
            return -ENODEV;
        if (off & (PAGE_SIZE - 1))
            return -EINVAL;
    } else {
        file = NULL;
    }
    if (len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EINVAL;
    len = PAGE_UP(len);
    uint32_t vflags = VM_READ | (prot & PROT_WRITE ? VM_WRITE : 0);

    if (flags & MAP_FIXED) {
        if (addr + len > KERNEL_VBASE || addr + len < addr)
            return -EINVAL;
        mm_remove(current_mm, addr, addr + len);    /* replaces what was there */
    } else {
        addr = mm_find_gap(current_mm, len);
        if (!addr)
            return -ENOMEM;
    }
    int err = mm_add_file(current_mm, addr, addr + len, vflags, file, off);
    return err ? err : (int32_t)addr;
}

int32_t mm_munmap(uint32_t addr, uint32_t len)
{
    if ((addr & (PAGE_SIZE - 1)) || len == 0 || addr + len > KERNEL_VBASE)
        return -EINVAL;
    return mm_remove(current_mm, addr, addr + PAGE_UP(len));
}

/* --- mprotect ---------------------------------------------------------- */

/* Make sure an area boundary falls at addr (split the area around it). */
static int split_at(struct mm *mm, uint32_t addr)
{
    struct vma *v = find(mm, addr);
    if (!v || v->start == addr)
        return 0;
    if (mm->nareas == MM_AREAS)
        return -ENOMEM;
    int i = v - mm->area;
    memmove(&mm->area[i + 1], &mm->area[i], (mm->nareas - i) * sizeof *v);
    mm->area[i].end = addr;
    mm->area[i + 1].file_off += addr - mm->area[i + 1].start;
    mm->area[i + 1].start = addr;
    mm->nareas++;
    return 0;
}

int32_t mm_mprotect(uint32_t addr, uint32_t len, uint32_t prot)
{
    struct mm *mm = current_mm;
    if ((addr & (PAGE_SIZE - 1)) || addr + len < addr)
        return -EINVAL;
    uint32_t end = PAGE_UP(addr + len);
    for (uint32_t a = addr; a < end; ) {        /* all of it must be mapped */
        struct vma *v = find(mm, a);
        if (!v)
            return -ENOMEM;
        a = v->end;
    }
    if (split_at(mm, addr) < 0 || split_at(mm, end) < 0)
        return -ENOMEM;
    uint32_t vflags = (prot & 1 ? VM_READ : 0) | (prot & PROT_WRITE ? VM_WRITE : 0) |
                      (prot & 4 ? VM_EXEC : 0);
    for (int i = 0; i < mm->nareas; i++)
        if (mm->area[i].start >= addr && mm->area[i].end <= end)
            mm->area[i].flags = vflags;
    /* Pages that exist get the new write permission (a copy-on-write page
       stays read-only: its fault handler makes it writable). */
    for (uint32_t a = addr; a < end; a += PAGE_SIZE) {
        uint32_t f = vmm_get_flags(a);
        if (!(f & PTE_PRESENT))
            continue;
        f &= ~PTE_WRITABLE;
        if ((vflags & VM_WRITE) && !(f & PTE_COW))
            f |= PTE_WRITABLE;
        vmm_protect(a, f);
    }
    return 0;
}
