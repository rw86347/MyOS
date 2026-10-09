/*
 * kheap.c — the kernel heap: kmalloc() / kfree() for any size
 *
 * pmm_alloc() only deals in whole 4 KiB pages. Most kernel objects are much
 * smaller (a file handle, a name, an inode...), so the heap carves pages up.
 * Two parts, because small and big allocations want different things:
 *
 * SMALL (up to 1 KiB) — size classes, O(1). This is where speed matters:
 *   the kernel allocates and frees small objects all the time.
 *   - 7 classes: 16, 32, 64, 128, 256, 512, 1024 bytes. A request is rounded
 *     up to the next class.
 *   - Each class has a free list: a chain of free objects, the link stored
 *     inside each free object itself (it's unused memory anyway).
 *   - kmalloc pops the first object off its class's list; kfree pushes it
 *     back on. No searching, ever.
 *   - When a class runs dry, it takes a fresh page from the PMM and cuts it
 *     into objects. Pages come through the direct map (P2V), so no page
 *     tables are touched. Each page starts with a 16-byte header saying
 *     which class it belongs to — that's how kfree knows the size.
 *   Linux's slab/SLUB allocators work on the same idea.
 *
 * BIG (over 1 KiB: thread stacks, buffers) — first fit, at KHEAP_START.
 *   - A virtual range that grows upward; physically scattered frames look
 *     contiguous through paging.
 *   - A row of blocks, each with a header, linked both ways:
 *       [hdr|  data  ][hdr|    data    ][hdr| data ] ...   heap_end
 *   - kmalloc walks the blocks for the first free one that fits (big
 *     allocations are rare, so the walk stays short) and splits it.
 *   - kfree merges the block with its free neighbours — O(1), thanks to
 *     the back links.
 *
 * kfree tells the two apart by address: small objects live in the direct map
 * (below KHEAP_START), big blocks above it.
 *
 * Pages are never handed back to the PMM; see docs/gaps.md.
 */
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>

#define ALIGN       16                  /* every allocation is 16-byte aligned */

/* --- small objects -------------------------------------------------------- */

#define NCLASSES    7
#define SMALL_MAX   1024
#define SLAB_MAGIC  0x534C4142          /* "SLAB" */

static const uint32_t class_size[NCLASSES] = { 16, 32, 64, 128, 256, 512, 1024 };

struct slab_page {                      /* the first 16 bytes of each page */
    uint32_t magic;
    uint32_t cls;
    uint32_t pad[2];
};

/* A free object: the link, and a mark that says "I'm on a free list".
   kfree of an object that already carries the mark is a double free —
   caught and refused, instead of putting the object on the list twice
   (which would hand it out to two owners). Objects are >= 16 bytes, so
   both words fit. */
#define FREE_MARK 0xF4EEF4EEu
struct free_obj { struct free_obj *next; uint32_t mark; };

static struct free_obj *free_list[NCLASSES];
static uint32_t small_in_use;           /* bytes handed out (for kheap_stats) */
static uint32_t pages_taken;            /* pages the heap got from the PMM */

static int class_of(uint32_t size)
{
    int c = 0;
    while (class_size[c] < size)
        c++;
    return c;                           /* at most 7 steps */
}

#ifdef MEM_DEBUG
/* make MEMDEBUG=1: a free object past its link and mark is 0xDEADBEEF
   (filled by kfree), and kmalloc checks it before handing the object out:
   a write after free, or an overrun from the object below, shows up as a
   changed word. pmm.c does the same for whole pages. */
#define DEAD 0xDEADBEEFu
static void obj_fill(struct free_obj *o, uint32_t size)
{
    uint32_t *w = (uint32_t *)(o + 1);
    for (uint32_t k = 0; k < (size - sizeof *o) / 4; k++)
        w[k] = DEAD;
}

static void obj_check(struct free_obj *o, uint32_t size)
{
    const uint32_t *w = (const uint32_t *)(o + 1);
    for (uint32_t k = 0; k < (size - sizeof *o) / 4; k++)
        if (w[k] != DEAD || o->mark != FREE_MARK) {
            kprintf("MEMDEBUG: a free %lu-byte object at %p was written: +%lx = %08lx "
                    "(mark %08lx)\n", (unsigned long)size, (void *)o,
                    (unsigned long)(sizeof *o + k * 4), (unsigned long)w[k],
                    (unsigned long)o->mark);
            return;
        }
}
#endif

/* Cut a fresh page into objects of class c and put them on its free list. */
static int refill(int c)
{
    uint32_t frame = pmm_alloc();
    if (!frame)
        return -1;
    pages_taken++;
    struct slab_page *page = P2V(frame);
    page->magic = SLAB_MAGIC;
    page->cls = c;

    uint32_t size = class_size[c];
    for (uint32_t off = sizeof *page; off + size <= PAGE_SIZE; off += size) {
        struct free_obj *o = (struct free_obj *)((uint8_t *)page + off);
#ifdef MEM_DEBUG
        obj_fill(o, size);
#endif
        o->mark = FREE_MARK;
        o->next = free_list[c];
        free_list[c] = o;
    }
    return 0;
}


static void *small_alloc(uint32_t size)
{
    int c = class_of(size);
    if (!free_list[c] && refill(c) < 0)
        return NULL;
    struct free_obj *o = free_list[c];
#ifdef MEM_DEBUG
    obj_check(o, class_size[c]);
#endif
    free_list[c] = o->next;
    o->mark = 0;
    small_in_use += class_size[c];
    return o;
}

static void small_free(void *ptr)
{
    struct slab_page *page = (struct slab_page *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
    if (page->magic != SLAB_MAGIC || page->cls >= NCLASSES) {
        kprintf("kfree: bad pointer %p\n", ptr);
        return;
    }
    struct free_obj *o = ptr;
    if (o->mark == FREE_MARK) {
        kprintf("kfree: %p freed twice (ignored)\n", ptr);
        return;
    }
#ifdef MEM_DEBUG
    obj_fill(o, class_size[page->cls]);
#endif
    o->mark = FREE_MARK;
    o->next = free_list[page->cls];
    free_list[page->cls] = o;
    small_in_use -= class_size[page->cls];
}

/* --- big blocks ------------------------------------------------------------ */

#define BLOCK_MAGIC 0x4B484540          /* "KHE@"; bit 0 set = the block is free */

struct block {
    uint32_t size;                      /* bytes of data after the header */
    struct block *prev, *next;          /* neighbours in memory (NULL at the ends) */
    uint32_t magic;                     /* BLOCK_MAGIC, | 1 if free */
};                                      /* 16 bytes, so data stays aligned */

static struct block *first, *last;
static uintptr_t heap_end = KHEAP_START; /* first unmapped address */

static int  is_free(const struct block *b) { return b->magic & 1; }
static int  is_block(const struct block *b) { return (b->magic & ~1u) == BLOCK_MAGIC; }

static uint32_t round_up(uint32_t n, uint32_t to)
{
    return (n + to - 1) & ~(to - 1);
}

/* Map enough new pages at heap_end to give at least `bytes` more room, and
   add the space to the last block (or start a new one). */
static int grow(uint32_t bytes)
{
    uintptr_t start = heap_end;
    uint32_t pages = round_up(bytes, PAGE_SIZE) / PAGE_SIZE;

    if (pages > (KHEAP_END - heap_end) / PAGE_SIZE)
        return -1;                      /* heap address range exhausted */
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t frame = pmm_alloc();
        if (!frame || vmm_map(heap_end, frame, PTE_WRITABLE) < 0) {
            if (frame)
                pmm_free(frame);
            break;                      /* keep whatever we did get */
        }
        heap_end += PAGE_SIZE;
        pages_taken++;
    }
    if (heap_end == start)
        return -1;

    uint32_t added = (uint32_t)(heap_end - start);
    if (last && is_free(last)) {
        last->size += added;            /* extend the free block at the end */
    } else {
        struct block *b = (struct block *)start;
        b->size  = added - sizeof *b;
        b->magic = BLOCK_MAGIC | 1;
        b->prev  = last;
        b->next  = NULL;
        if (last)
            last->next = b;
        else
            first = b;
        last = b;
    }
    return heap_end - start >= bytes ? 0 : -1;
}

static void *big_alloc(uint32_t size)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = first; b; b = b->next) {
            if (!is_free(b) || b->size < size)
                continue;
            /* Split if the leftover can hold a header + a minimum block. */
            if (b->size >= size + sizeof(struct block) + ALIGN) {
                struct block *rest = (struct block *)((uint8_t *)(b + 1) + size);
                rest->size  = b->size - size - sizeof(struct block);
                rest->magic = BLOCK_MAGIC | 1;
                rest->prev  = b;
                rest->next  = b->next;
                if (b->next)
                    b->next->prev = rest;
                else
                    last = rest;
                b->next = rest;
                b->size = size;
            }
            b->magic = BLOCK_MAGIC;     /* in use */
            return b + 1;               /* data starts right after the header */
        }
        /* Nothing fits: grow the heap and search once more. */
        if (grow(size + sizeof(struct block)) < 0)
            return NULL;
    }
    return NULL;
}

/* Swallow b's right neighbour n (both free) into b. */
static void merge_next(struct block *b)
{
    struct block *n = b->next;
    b->size += sizeof(struct block) + n->size;
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
    else
        last = b;
    n->magic = 0;
}

static void big_free(void *ptr)
{
    struct block *b = (struct block *)ptr - 1;
    if (!is_block(b) || is_free(b)) {
        kprintf("kfree: bad pointer or double free: %p\n", ptr);
        return;
    }
    b->magic |= 1;
    if (b->next && is_free(b->next))
        merge_next(b);
    if (b->prev && is_free(b->prev))
        merge_next(b->prev);
}

/* --- the interface ------------------------------------------------------- */

/*
 * `make test` builds fill every allocation with a pattern that is not a valid
 * pointer and not a NUL, because the one bug class no sanitiser here can see
 * is memory that was validly allocated, never written, and then used -- or
 * handed to user space. Zero-filled memory hides it: an uninitialised
 * pointer reads as NULL, every loop looking for a terminator stops at once,
 * and the test passes. That happened: execve with a NULL argv walked an
 * unterminated vector of stale heap, and the test for it could not tell,
 * because the recycled block read back as zeroes.
 *
 * 0xA5 was chosen so a word of it (0xA5A5A5A5) is above KERNEL_VBASE but
 * unmapped, so dereferencing one faults instead of quietly working.
 *
 * Test builds only: a normal kmalloc does not touch the memory it returns.
 */
#ifdef SELFTEST
#define HEAP_POISON 0xA5
static void poison(void *p, uint32_t n)
{
    if (p)
        memset(p, HEAP_POISON, n);
}
#else
#define poison(p, n) ((void)0)
#endif

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    void *p = size <= SMALL_MAX ? small_alloc(size)
                                : big_alloc(round_up(size, ALIGN));
    poison(p, size);
    return p;
}

/* --- physically contiguous blocks ----------------------------------------- */

/* Linux drivers expect kmalloc memory to be physically contiguous (they
   hand it to devices for DMA), and our big blocks are not: their pages are
   scattered, only the virtual addresses line up. So the Linux API's
   kmalloc (kernel/linux_slab.c) takes bigger blocks as a run of pages from
   the PMM, seen through the direct map. The first 16 bytes say so — the
   same place a slab page keeps its header — so kfree can tell. */
#define CONT_MAGIC  0x434F4E54          /* "CONT" */

struct cont_page {
    uint32_t magic;
    uint32_t pages;
    uint32_t pad[2];
};

void *kmalloc_contig(size_t size)
{
    uint32_t pages = (uint32_t)((size + sizeof(struct cont_page) + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t pa = pages == 1 ? pmm_alloc() : pmm_alloc_run(pages);
    if (!pa)
        return NULL;
    struct cont_page *h = P2V(pa);
    h->magic = CONT_MAGIC;
    h->pages = pages;
    return h + 1;
}

static struct cont_page *cont_of(const void *ptr)
{
    struct cont_page *h = (struct cont_page *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
    return h->magic == CONT_MAGIC && (const void *)(h + 1) == ptr ? h : NULL;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    if ((uintptr_t)ptr < KHEAP_START) {
        struct cont_page *h = cont_of(ptr);
        if (h) {                        /* a contiguous run */
            uint32_t pa = (uint32_t)V2P(h), pages = h->pages;
            h->magic = 0;
            for (uint32_t k = 0; k < pages; k++)
                pmm_free(pa + k * PAGE_SIZE);
        } else {
            small_free(ptr);            /* in the direct map: a size class */
        }
    } else {
        big_free(ptr);
    }
}

size_t kheap_size(const void *ptr)
{
    if (!ptr)
        return 0;
    if ((uintptr_t)ptr < KHEAP_START && cont_of(ptr))
        return cont_of(ptr)->pages * PAGE_SIZE - sizeof(struct cont_page);
    if ((uintptr_t)ptr < KHEAP_START) {     /* the class, from the page's header */
        const struct slab_page *page =
            (const struct slab_page *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));
        return class_size[page->cls];
    }
    return ((const struct block *)ptr - 1)->size;
}

void kheap_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *mapped)
{
    *used = small_in_use;
    *free_bytes = 0;
    for (struct block *b = first; b; b = b->next) {
        if (is_free(b))
            *free_bytes += b->size;
        else
            *used += b->size;
    }
    *mapped = (uint32_t)(heap_end - KHEAP_START);
}

uint32_t kheap_pages(void)
{
    return pages_taken;
}
