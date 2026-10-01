/*
 * kheap.c — the kernel heap: kmalloc() / kfree() for any size
 *
 * pmm_alloc() only deals in whole 4 KiB pages. Most kernel objects are much
 * smaller (a process struct, a file handle, a name...), so the heap carves
 * pages into blocks of any size.
 *
 * Design (deliberately simple — a "first-fit free list"):
 *   - The heap is a virtual range starting at KHEAP_START (0xF0000000).
 *     It grows upward: when we need more room, we take frames from the PMM
 *     and map them at the end (heap_end). The physical frames don't need to
 *     be next to each other — paging makes them look contiguous.
 *   - The heap is a row of blocks, each with a small header in front of it:
 *
 *       [hdr|  data  ][hdr|    data    ][hdr| data ] ...   heap_end
 *
 *     The blocks always tile the whole heap exactly, so each header's
 *     `next` is simply the block right after it in memory.
 *   - kmalloc: walk the blocks, take the first free one that is big enough,
 *     and split off the rest if it's large. If none fits, grow the heap.
 *   - kfree: mark the block free and merge it with free neighbours, so the
 *     heap doesn't crumble into many tiny unusable pieces ("fragmentation").
 *
 * Memory is never handed back to the PMM; fine for now.
 * Linux uses much fancier allocators (slab/SLUB); this is the textbook one.
 */
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>

#define ALIGN       16                  /* every block's data is 16-byte aligned */
#define HEAP_MAGIC  0x4B484541          /* "KHEA": detects corrupted headers */

struct block {
    uint32_t size;                      /* bytes of data after the header */
    uint32_t free;
    struct block *next;                 /* next block in memory, NULL if last */
    uint32_t magic;
};                                      /* 16 bytes, so data stays aligned */

static struct block *first, *last;
static uint32_t heap_end = KHEAP_START;  /* first unmapped address */

static uint32_t round_up(uint32_t n, uint32_t to)
{
    return (n + to - 1) & ~(to - 1);
}

/* Map enough new pages at heap_end to give at least `bytes` more room, and
   add the space to the last block (or start a new one). */
static int grow(uint32_t bytes)
{
    uint32_t start = heap_end;
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
    }
    if (heap_end == start)
        return -1;

    uint32_t added = heap_end - start;
    if (last && last->free) {
        last->size += added;            /* extend the free block at the end */
    } else {
        struct block *b = (struct block *)start;
        b->size  = added - sizeof *b;
        b->free  = 1;
        b->next  = NULL;
        b->magic = HEAP_MAGIC;
        if (last)
            last->next = b;
        else
            first = b;
        last = b;
    }
    return heap_end - start >= bytes ? 0 : -1;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    size = round_up(size, ALIGN);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = first; b; b = b->next) {
            if (!b->free || b->size < size)
                continue;
            /* Split if the leftover can hold a header + a minimum block. */
            if (b->size >= size + sizeof(struct block) + ALIGN) {
                struct block *rest = (struct block *)((uint8_t *)(b + 1) + size);
                rest->size  = b->size - size - sizeof(struct block);
                rest->free  = 1;
                rest->next  = b->next;
                rest->magic = HEAP_MAGIC;
                b->next = rest;
                b->size = size;
                if (last == b)
                    last = rest;
            }
            b->free = 0;
            return b + 1;               /* data starts right after the header */
        }
        /* Nothing fits: grow the heap and search once more. */
        if (grow(size + sizeof(struct block)) < 0)
            return NULL;
    }
    return NULL;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct block *b = (struct block *)ptr - 1;
    if (b->magic != HEAP_MAGIC || b->free) {
        kprintf("kfree: bad pointer or double free: %p\n", ptr);
        return;
    }
    b->free = 1;

    /* Merge every run of neighbouring free blocks into one. */
    for (struct block *x = first; x; x = x->next) {
        while (x->free && x->next && x->next->free) {
            struct block *n = x->next;
            x->size += sizeof(struct block) + n->size;
            x->next = n->next;
            n->magic = 0;
            if (last == n)
                last = x;
        }
    }
}

void kheap_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *mapped)
{
    *used = *free_bytes = 0;
    for (struct block *b = first; b; b = b->next) {
        if (b->free)
            *free_bytes += b->size;
        else
            *used += b->size;
    }
    *mapped = heap_end - KHEAP_START;
}
