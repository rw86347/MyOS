/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * linux_slab.c — kmalloc(size, gfp) and friends for Linux drivers, on the
 * kernel heap (mm/kheap.c). Kept apart from include/linux/slab.h: the
 * kernel's own kmalloc takes one argument and Linux's two, so the two
 * names can't share a file. kfree is the same function in both.
 */
#include <kheap.h>
#include <linux/gfp.h>
#include <string.h>

void *__kmalloc(size_t size, unsigned int gfp);
void *__vmalloc(size_t size, unsigned int gfp);
void *krealloc(const void *p, size_t size, unsigned int gfp);
size_t ksize(const void *p);
void *kmemdup(const void *src, size_t len, unsigned int gfp);
char *kstrdup(const char *s, unsigned int gfp);
char *kstrndup(const char *s, size_t max, unsigned int gfp);
void *kmemdup_nul(const char *s, size_t len, unsigned int gfp);

/* Up to 1 KiB from the size classes (an object never crosses its page);
   bigger as a contiguous page run — so, as in Linux, any kmalloc buffer
   can be handed to a device for DMA. */
void *__kmalloc(size_t size, unsigned int gfp)
{
    if (!size)
        size = 1;                       /* Linux hands out a valid pointer for 0 */
    void *p = size <= 1024 ? kmalloc(size) : kmalloc_contig(size);
    if (p && (gfp & __GFP_ZERO))
        memset(p, 0, size);
    return p;
}

/* vmalloc: virtually contiguous only (the heap's big blocks), as in Linux. */
void *__vmalloc(size_t size, unsigned int gfp)
{
    void *p = kmalloc(size ? size : 1);
    if (p && (gfp & __GFP_ZERO))
        memset(p, 0, size);
    return p;
}

size_t ksize(const void *p)
{
    return kheap_size(p);
}

void *krealloc(const void *p, size_t size, unsigned int gfp)
{
    if (!p)
        return __kmalloc(size, gfp);
    size_t have = kheap_size(p);
    if (size <= have)
        return (void *)p;               /* it already fits */
    void *n = __kmalloc(size, gfp);
    if (n) {
        memcpy(n, p, have);
        if (gfp & __GFP_ZERO)
            memset((char *)n + have, 0, size - have);
        kfree((void *)p);
    }
    return n;
}

void *kmemdup(const void *src, size_t len, unsigned int gfp)
{
    void *p = __kmalloc(len, gfp & ~__GFP_ZERO);
    if (p)
        memcpy(p, src, len);
    return p;
}

char *kstrndup(const char *s, size_t max, unsigned int gfp)
{
    if (!s)
        return NULL;
    size_t n = 0;
    while (n < max && s[n])
        n++;
    char *p = __kmalloc(n + 1, gfp & ~__GFP_ZERO);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

char *kstrdup(const char *s, unsigned int gfp)
{
    return s ? kstrndup(s, strlen(s), gfp) : NULL;
}

void *kmemdup_nul(const char *s, size_t len, unsigned int gfp)
{
    return kstrndup(s, len, gfp);
}

/* kmem_cache: a named size; every object comes from kmalloc. The size
   classes already make same-sized objects O(1), which is what Linux's
   caches are for. */
struct kmem_cache {
    unsigned int size;
    void (*ctor)(void *);
};

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
                                     unsigned int flags, void (*ctor)(void *))
{
    (void)name; (void)align; (void)flags;
    struct kmem_cache *c = kmalloc(sizeof *c);
    if (c) {
        c->size = size;
        c->ctor = ctor;
    }
    return c;
}

void kmem_cache_destroy(struct kmem_cache *c)
{
    kfree(c);
}

void *kmem_cache_alloc(struct kmem_cache *c, unsigned int gfp)
{
    void *p = __kmalloc(c->size, gfp);
    if (p && c->ctor)
        c->ctor(p);
    return p;
}

void kmem_cache_free(struct kmem_cache *c, void *p)
{
    (void)c;
    kfree(p);
}
