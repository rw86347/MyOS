/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * kheap.c — slab-only kmalloc for ARM64 bring-up (identity map, no vmm_map yet)
 */
#include <kheap.h>
#include <memlayout.h>
#include <pmm.h>
#include <string.h>

#define NCLASSES    7
#define SMALL_MAX   1024
#define SLAB_MAGIC  0x534C4142u

static const uint32_t class_size[NCLASSES] = { 16, 32, 64, 128, 256, 512, 1024 };

struct slab_page {
    uint32_t magic;
    uint32_t cls;
    uint32_t pad[2];
};

#define FREE_MARK 0xF4EEF4EEu
struct free_obj {
    struct free_obj *next;
    uint32_t mark;
};

static struct free_obj *free_list[NCLASSES];
static uint32_t small_in_use;
static uint32_t pages_taken;

static int class_of(uint32_t size)
{
    int c = 0;

    while (class_size[c] < size)
        c++;
    return c;
}

static int refill(int c)
{
    uint32_t frame = pmm_alloc();

    if (!frame)
        return -1;
    pages_taken++;
    struct slab_page *page = P2V(frame);

    page->magic = SLAB_MAGIC;
    page->cls = (uint32_t)c;

    uint32_t size = class_size[c];
    for (uint32_t off = sizeof *page; off + size <= PAGE_SIZE; off += size) {
        struct free_obj *o = (struct free_obj *)((uint8_t *)page + off);

        o->mark = FREE_MARK;
        o->next = free_list[c];
        free_list[c] = o;
    }
    return 0;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    if (size > SMALL_MAX)
        return NULL;

    int c = class_of((uint32_t)size);

    if (!free_list[c] && refill(c) < 0)
        return NULL;
    struct free_obj *o = free_list[c];

    free_list[c] = o->next;
    o->mark = 0;
    small_in_use += class_size[c];
    return o;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct slab_page *page =
        (struct slab_page *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));

    if (page->magic != SLAB_MAGIC || page->cls >= NCLASSES)
        return;
    struct free_obj *o = ptr;

    if (o->mark == FREE_MARK)
        return;
    o->mark = FREE_MARK;
    o->next = free_list[page->cls];
    free_list[page->cls] = o;
    small_in_use -= class_size[page->cls];
}

void *kmalloc_contig(size_t size)
{
    (void)size;
    return NULL;
}

size_t kheap_size(const void *ptr)
{
    if (!ptr)
        return 0;
    struct slab_page *page =
        (struct slab_page *)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));

    if (page->magic != SLAB_MAGIC || page->cls >= NCLASSES)
        return 0;
    return class_size[page->cls];
}

uint32_t kheap_pages(void)
{
    return pages_taken;
}

void kheap_stats(uint32_t *used, uint32_t *free_bytes, uint32_t *mapped)
{
    if (used)
        *used = small_in_use;
    if (free_bytes)
        *free_bytes = pages_taken * PAGE_SIZE - small_in_use;
    if (mapped)
        *mapped = pages_taken * PAGE_SIZE;
}
