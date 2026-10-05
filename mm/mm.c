/*
 * mm.c — address spaces: areas, demand paging, brk, mmap (see mm.h)
 *
 * The CPU-specific work (page directories, copy-on-write bits) is in
 * arch/<cpu>/paging.c; this file decides what is allowed where.
 *
 * Speed: an address space's areas are one array, kept sorted, so a lookup
 * (every page fault) is a binary search: 5 steps for 32 areas, 9 for 400.
 * A small program has 4-6 areas (code, data, heap, stack, a few mmaps); each
 * shared library adds 4-5, so a GTK program has hundreds. The array starts
 * with room for MM_AREAS_FIRST (896 bytes, a kmalloc size class) and
 * doubles when full, up to MM_AREAS_MAX. Growing copies it: a pointer into
 * the array is stale after anything that adds an area.
 */
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <mm.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
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
    /* Remember it with the thread: if it sleeps now (loading a program
       from disk), it must get THIS address space back when it wakes, not
       its process's usual one (execve loads into a new one). */
    struct thread *t = thread_current();
    if (t)
        t->mm = mm;
}

struct mm *mm_create(void)
{
    struct mm *mm = kmalloc(sizeof *mm);
    if (!mm)
        return NULL;
    memset(mm, 0, sizeof *mm);
    mm->area = kmalloc(MM_AREAS_FIRST * sizeof *mm->area);
    mm->cap = MM_AREAS_FIRST;
    mm->pd = mm->area ? vmm_pd_create() : 0;
    if (!mm->pd) {
        kfree(mm->area);
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
    memcpy(child, mm, sizeof *child);   /* same brk */
    child->cap = mm->cap ? mm->cap : MM_AREAS_FIRST;
    child->area = kmalloc(child->cap * sizeof *mm->area);
    child->pd = child->area ? vmm_pd_fork() : 0;    /* same pages, copy-on-write */
    if (!child->pd) {
        kfree(child->area);
        kfree(child);
        return NULL;
    }
    memcpy(child->area, mm->area, mm->nareas * sizeof *mm->area);   /* same areas */
    for (int i = 0; i < child->nareas; i++)
        if (child->area[i].file)
            vfs_get(child->area[i].file);   /* the child maps the files too */
    return child;
}

void mm_destroy(struct mm *mm)
{
    for (int i = 0; i < mm->nareas; i++)
        if (mm->area[i].file)
            vfs_put(mm->area[i].file);
    vmm_pd_destroy(mm->pd);
    kfree(mm->area);
    kfree(mm);
}

/* The area containing addr, or NULL. */
static struct vma *find(struct mm *mm, uint32_t addr)
{
    int lo = 0, hi = mm->nareas;        /* the answer is in [lo, hi) */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        struct vma *v = &mm->area[mid];
        if (addr < v->start)
            hi = mid;
        else if (addr >= v->end)
            lo = mid + 1;
        else
            return v;
    }
    return NULL;
}

/* The first area that ends after addr (so it contains addr or is the
   next one up), as an index; nareas if none. */
static int first_after(struct mm *mm, uint32_t addr)
{
    int lo = 0, hi = mm->nareas;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (mm->area[mid].end <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int overlaps(struct mm *mm, uint32_t start, uint32_t end)
{
    int i = first_after(mm, start);
    return i < mm->nareas && mm->area[i].start < end;
}

/* Make room for one more area: double the array if it's full. Moves the
   areas, so pointers into the array are stale afterwards. */
static int room_for_one(struct mm *mm)
{
    if (mm->nareas < mm->cap)
        return 0;
    int cap = mm->cap ? 2 * mm->cap : MM_AREAS_FIRST;
    if (cap > MM_AREAS_MAX)
        return -ENOMEM;
    struct vma *a = kmalloc(cap * sizeof *a);
    if (!a)
        return -ENOMEM;
    memcpy(a, mm->area, mm->nareas * sizeof *a);
    kfree(mm->area);
    mm->area = a;
    mm->cap = cap;
    return 0;
}

int mm_add(struct mm *mm, uint32_t start, uint32_t end, uint32_t flags)
{
    return mm_add_file(mm, start, end, flags, NULL, 0, 0);
}

/* Add area *n ([n->start, n->end), its flags and file part). */
static int add_area(struct mm *mm, const struct vma *n)
{
    uint32_t start = n->start, end = n->end, flags = n->flags;
    struct inode *file = n->file;
    if (start >= end || end > KERNEL_VBASE || overlaps(mm, start, end))
        return -EINVAL;

    /* Grow a neighbour instead of adding an area, if it touches and has the
       same flags (the heap growing with brk does this all the time). A file
       area grows only when the next page is the next bytes of the same file
       with the same end-of-data, so one ELF segment is one area. */
    for (int i = 0; i < mm->nareas; i++) {
        struct vma *v = &mm->area[i];
        if (v->flags != flags || v->file != file || v->file_end != n->file_end ||
            v->phys != n->phys)
            continue;
        if (file && v->file_off + (v->end - v->start) != n->file_off)
            continue;
        if (v->end == start && (i + 1 == mm->nareas || mm->area[i + 1].start >= end)) {
            v->end = end;
            return 0;
        }
        if (!file && v->start == end) {
            v->start = start;
            return 0;
        }
    }

    if (room_for_one(mm) < 0)
        return -ENOMEM;
    int i = mm->nareas;
    while (i > 0 && mm->area[i - 1].start > start) {    /* keep them sorted */
        mm->area[i] = mm->area[i - 1];
        i--;
    }
    mm->area[i] = *n;
    mm->nareas++;
    if (file)
        vfs_get(file);      /* a mapped file stays readable, even if deleted */
    return 0;
}

int mm_add_file(struct mm *mm, uint32_t start, uint32_t end, uint32_t flags,
                struct inode *file, uint32_t file_off, uint32_t file_end)
{
    struct vma n = { .start = start, .end = end, .flags = flags, .file = file,
                     .file_off = file_off, .file_end = file_end };
    return add_area(mm, &n);
}

int mm_remove(struct mm *mm, uint32_t start, uint32_t end)
{
    for (int i = 0; i < mm->nareas; i++) {
        struct vma *v = &mm->area[i];
        if (v->end <= start || end <= v->start)
            continue;
        if (start > v->start && end < v->end) {         /* a hole in the middle */
            if (room_for_one(mm) < 0)
                return -ENOMEM;
            v = &mm->area[i];
            memmove(&mm->area[i + 2], &mm->area[i + 1],
                    (mm->nareas - i - 1) * sizeof *v);
            mm->area[i + 1] = *v;
            mm->area[i + 1].start = end;
            mm->area[i + 1].file_off = v->file_off + (end - v->start);
            if (v->file)
                vfs_get(v->file);
            v->end = start;
            mm->nareas++;
            break;
        }
        if (start <= v->start && end >= v->end) {       /* all of it */
            if (v->file)
                vfs_put(v->file);
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

    /* Device memory: map the device's page. Nothing to allocate or copy. */
    if (v->flags & VM_PFNMAP) {
        uint32_t page = PAGE_DOWN(va), off = v->file_off + (page - v->start);
        if (off >= v->file_end)
            return -1;                  /* past the device: SIGSEGV (Linux: SIGBUS) */
        uint32_t flags = PTE_USER | PTE_SHARED | (v->flags & VM_WRITE ? PTE_WRITABLE : 0) |
                         (v->flags & VM_WC ? pte_write_combining() : PTE_PCD);
        return vmm_map(page, v->phys + off, flags) < 0 ? -1 : 0;
    }

    /* Shared memory: map the segment's own page, made on the first touch
       by anyone. PTE_SHARED: fork doesn't make it copy-on-write, and
       unmapping doesn't free it. */
    if (v->flags & VM_SHARED) {
        uint32_t page = PAGE_DOWN(va), off = v->file_off + (page - v->start);
        uint32_t frame = off < PAGE_UP(v->file->size) ? ramfs_page(v->file, off / PAGE_SIZE) : 0;
        if (!frame)
            return -1;                  /* past its end, or out of memory */
        uint32_t flags = PTE_USER | PTE_SHARED | (v->flags & VM_WRITE ? PTE_WRITABLE : 0);
        return vmm_map(page, frame, flags) < 0 ? -1 : 0;
    }

    /* Demand paging: the first touch of a page in an area creates it —
       zero-filled, or for a mapped file, a copy of that part of the file
       (straight from the initramfs, or from its RAM pages). Pages never
       touched cost nothing. */
    uint32_t frame = pmm_alloc();
    if (!frame)
        return -1;                      /* out of memory: the program dies */
    uint32_t page = PAGE_DOWN(va), n = 0;
    if (v->file) {
        uint32_t off = v->file_off + (page - v->start);
        /* ~0: mmap of the whole file. An ELF segment stops at file_end so
           the bytes after p_filesz (the BSS) stay zero. */
        uint32_t limit = v->file_end == ~0u ? v->file->size : v->file_end;
        if (limit > v->file->size)
            limit = v->file->size;
        if (off < limit)
            n = limit - off < PAGE_SIZE ? limit - off : PAGE_SIZE;
        if (n) {
            /* The copy may sleep on the disk. Meanwhile another thread of
               this process can munmap the area (dropping the file) or
               fault the same page in: hold the file, and look again. */
            struct inode *file = v->file;
            vfs_get(file);
            int err = file_copy_out(file, off, P2V(frame), n);
            v = find(current_mm, va);
            int same = v && v->file == file && v->file_off + (page - v->start) == off;
            vfs_put(file);
            if (err < 0) {
                pmm_free(frame);
                return -1;
            }
            if (!same || vmm_translate(page)) {
                pmm_free(frame);
                return 0;               /* run it again: it faults afresh */
            }
        }
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
#define MAP_SHARED    0x01
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
                struct inode *file, uint32_t off, uint32_t fmode)
{
    if (!(flags & MAP_ANONYMOUS)) {
        /* A file, mapped privately: writes change our copy, never the
           file (MAP_SHARED is treated the same: see docs/gaps.md). */
        if (!file || !S_ISREG(file->mode) || (file->ops && !file->fs))
            return -ENODEV;
        if (off & (PAGE_SIZE - 1))
            return -EINVAL;
        /* MAP_PRIVATE writes a copy: the dynamic linker opens every .so
           O_RDONLY and maps its data PROT_WRITE so it can relocate. Only
           MAP_SHARED writes would reach the file, and those need write. */
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE) &&
            (fmode & O_ACCMODE) == O_RDONLY)
            return -EACCES;
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
    int err = mm_add_file(current_mm, addr, addr + len, vflags, file, off, ~0u);
    return err ? err : (int32_t)addr;
}

int vm_iomap_memory(struct vma *vma, uint32_t phys, uint32_t size)
{
    uint32_t len = vma->end - vma->start;
    if (vma->file_off > size || len > size - vma->file_off)
        return -EINVAL;
    vma->flags |= VM_PFNMAP;
    vma->phys = phys;
    vma->file_end = size;
    return 0;
}

int32_t mm_mmap_device(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags,
                       struct file *f, uint32_t off)
{
    if (len == 0 || ((addr | off) & (PAGE_SIZE - 1)))
        return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return -EACCES;
    if ((prot & PROT_WRITE) && (f->flags & O_ACCMODE) != O_RDWR)
        return -EACCES;
    len = PAGE_UP(len);
    if (len == 0)
        return -EINVAL;
    struct vma n = { .start = 0, .end = len, .file = f->inode, .file_off = off,
                     .flags = VM_READ | (prot & PROT_WRITE ? VM_WRITE : 0) |
                              ((f->flags & O_ACCMODE) == O_RDWR ? VM_MAYWRITE : 0) };
    int err = f->ops->mmap(f, &n);
    if (err < 0)
        return err;
    if (!(n.flags & VM_PFNMAP))
        return -ENODEV;

    if (flags & MAP_FIXED) {
        if (addr + len > KERNEL_VBASE || addr + len < addr)
            return -EINVAL;
        mm_remove(current_mm, addr, addr + len);
    } else {
        addr = mm_find_gap(current_mm, len);
        if (!addr)
            return -ENOMEM;
    }
    n.start = addr;
    n.end = addr + len;
    err = add_area(current_mm, &n);
    return err ? err : (int32_t)addr;
}

int32_t mm_munmap(uint32_t addr, uint32_t len)
{
    if ((addr & (PAGE_SIZE - 1)) || len == 0 || addr + len > KERNEL_VBASE)
        return -EINVAL;
    return mm_remove(current_mm, addr, addr + PAGE_UP(len));
}

/* mremap: grow or shrink a mapping, or move it if MREMAP_MAYMOVE.
   Firefox's allocator and xterm both grow anonymous maps this way.
   One whole area only: a piece of a map, MREMAP_DONTUNMAP, and a
   FIXED move that overlaps the old range wait for a user. */
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

int32_t mm_mremap(uint32_t old, uint32_t old_len, uint32_t new_len,
                  uint32_t flags, uint32_t new_addr)
{
    struct mm *mm = current_mm;
    if ((old & (PAGE_SIZE - 1)) || old_len == 0)
        return -EINVAL;
    if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED))
        return -EINVAL;
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
        return -EINVAL;
    old_len = PAGE_UP(old_len);
    new_len = PAGE_UP(new_len);
    if (new_len == 0)
        return -EINVAL;

    struct vma *v = find(mm, old);
    if (!v || v->start != old || v->end != old + old_len)
        return -EFAULT;
    if (v->flags & VM_PFNMAP)
        return -EINVAL;

    if (new_len == old_len && !(flags & MREMAP_FIXED))
        return (int32_t)old;

    if (new_len < old_len && !(flags & MREMAP_FIXED)) {
        mm_remove(mm, old + new_len, old + old_len);
        return (int32_t)old;
    }

    if (new_len > old_len && !(flags & MREMAP_FIXED)) {
        uint32_t grow_to = old + new_len;
        if (grow_to > KERNEL_VBASE || grow_to < old)
            return -ENOMEM;
        int i = (int)(v - mm->area);
        if (i + 1 == mm->nareas || mm->area[i + 1].start >= grow_to) {
            v->end = grow_to;
            return (int32_t)old;
        }
        if (!(flags & MREMAP_MAYMOVE))
            return -ENOMEM;
    }

    if (!(flags & MREMAP_MAYMOVE))
        return -ENOMEM;

    uint32_t dest;
    if (flags & MREMAP_FIXED) {
        if ((new_addr & (PAGE_SIZE - 1)) || new_addr + new_len > KERNEL_VBASE ||
            new_addr + new_len < new_addr)
            return -EINVAL;
        dest = new_addr;
        if (dest < old + old_len && dest + new_len > old)
            return -EINVAL;             /* overlapping FIXED: later */
        mm_remove(mm, dest, dest + new_len);
        v = find(mm, old);              /* mm_remove can move the array */
        if (!v || v->start != old)
            return -EFAULT;
    } else {
        dest = mm_find_gap(mm, new_len);
        if (!dest)
            return -ENOMEM;
        v = find(mm, old);
        if (!v)
            return -EFAULT;
    }

    struct vma nv = *v;
    nv.start = dest;
    nv.end = dest + new_len;
    if (add_area(mm, &nv) < 0)
        return -ENOMEM;

    uint32_t copy = old_len < new_len ? old_len : new_len;
    for (uint32_t off = 0; off < copy; off += PAGE_SIZE) {
        uint32_t src = old + off;
        uint32_t fl = vmm_get_flags(src);
        if (!(fl & PTE_PRESENT))
            continue;
        uint32_t pa = vmm_unmap(src);
        if (!pa)
            continue;
        if (vmm_map(dest + off, pa, fl) < 0) {
            if (!(fl & PTE_SHARED))
                pmm_free(pa);
            mm_remove(mm, dest, dest + new_len);
            return -ENOMEM;
        }
    }
    mm_remove(mm, old, old + old_len);
    return (int32_t)dest;
}

/* --- mprotect ---------------------------------------------------------- */

/* Make sure an area boundary falls at addr (split the area around it). */
static int split_at(struct mm *mm, uint32_t addr)
{
    struct vma *v = find(mm, addr);
    if (!v || v->start == addr)
        return 0;
    int i = v - mm->area;
    if (room_for_one(mm) < 0)
        return -ENOMEM;
    memmove(&mm->area[i + 1], &mm->area[i], (mm->nareas - i) * sizeof *v);
    mm->area[i].end = addr;
    mm->area[i + 1].file_off += addr - mm->area[i + 1].start;
    mm->area[i + 1].start = addr;
    mm->nareas++;
    if (mm->area[i].file)
        vfs_get(mm->area[i].file);      /* two areas map it now */
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
        if ((v->flags & (VM_PFNMAP | VM_SHARED)) && (prot & PROT_WRITE) &&
            !(v->flags & VM_MAYWRITE))
            return -EACCES;             /* a device opened, or a segment attached, read-only */
        a = v->end;
    }
    if (split_at(mm, addr) < 0 || split_at(mm, end) < 0)
        return -ENOMEM;
    uint32_t vflags = (prot & 1 ? VM_READ : 0) | (prot & PROT_WRITE ? VM_WRITE : 0) |
                      (prot & 4 ? VM_EXEC : 0);
    for (int i = 0; i < mm->nareas; i++)
        if (mm->area[i].start >= addr && mm->area[i].end <= end)
            mm->area[i].flags = vflags | (mm->area[i].flags &
                                          (VM_PFNMAP | VM_WC | VM_MAYWRITE | VM_SHARED));
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

/* --- shmdt ----------------------------------------------------------------- */

struct inode *mm_shm_detach(uint32_t addr)
{
    struct mm *mm = current_mm;
    struct vma *v = find(mm, addr);
    if (!v || !(v->flags & VM_SHARED) || v->start != addr || v->file_off != 0)
        return NULL;
    /* mprotect may have split the attach into several areas: the ones of
       this segment whose offset says they belong to the attach at addr.
       Anything else in the range stays — something mapped over part of
       it, or another attach of the same segment (offset 0 again). */
    struct inode *seg = v->file;
    uint32_t end = addr + PAGE_UP(seg->size);
    vfs_get(seg);
    for (int i = first_after(mm, addr); i < mm->nareas && mm->area[i].start < end; ) {
        struct vma *a = &mm->area[i];
        if (a->file == seg && (a->flags & VM_SHARED) && a->file_off == a->start - addr)
            mm_remove(mm, a->start, a->end);    /* the next area moves down to i */
        else
            i++;
    }
    return seg;
}
