/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_SLAB_H
#define LINUX_SLAB_H

/* Memory with Linux's names: kmalloc(size, gfp) and friends, on the
   kernel heap (mm/kheap.c: small objects O(1) in size classes, bigger
   ones first fit). The gfp flags say how hard Linux may try; ours never
   sleeps to find memory, so only __GFP_ZERO changes anything. vmalloc is
   the same heap: it is virtually contiguous already. */
#include <linux/types.h>

#include <linux/gfp.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/cleanup.h>

void *__kmalloc(size_t size, gfp_t gfp);
void  kfree(const void *p);
void *krealloc(const void *p, size_t size, gfp_t gfp);
size_t ksize(const void *p);
#define kmalloc(size, gfp)          __kmalloc((size), (gfp))
#define kzalloc(size, gfp)          __kmalloc((size), (gfp) | __GFP_ZERO)
#define kmalloc_array(n, size, gfp) ((size) && (n) > (size_t)-1 / (size) ? NULL : __kmalloc((n) * (size), (gfp)))
#define kcalloc(n, size, gfp)       kmalloc_array((n), (size), (gfp) | __GFP_ZERO)
#define kmalloc_node(s, gfp, node)  kmalloc(s, gfp)
#define kzalloc_node(s, gfp, node)  kzalloc(s, gfp)
#define kvmalloc(s, gfp)            kmalloc(s, gfp)
#define kvzalloc(s, gfp)            kzalloc(s, gfp)
#define kvcalloc(n, s, gfp)         kcalloc(n, s, gfp)
#define kvmalloc_array(n, s, gfp)   kmalloc_array(n, s, gfp)
void *__vmalloc(size_t size, gfp_t gfp);  /* not physically contiguous */
#define vmalloc(s)                  __vmalloc((s), GFP_KERNEL)
#define vzalloc(s)                  __vmalloc((s), GFP_KERNEL | __GFP_ZERO)
#define vfree(p)                    kfree(p)
#define kvfree(p)                   kfree(p)
#define kfree_sensitive(p)          kfree(p)
#define kvfree_sensitive(p, n)      kfree(p)
/* Linux 7's typed allocations: kzalloc_obj(*p) or kzalloc_obj(struct x)
   returns a pointer of the right type; the gfp flags are optional
   (GFP_KERNEL). Sizes go through overflow.h, so a huge count fails. */
#define __default_gfp(g, ...)       g
#define default_gfp(...)            __default_gfp(__VA_ARGS__ __VA_OPT__(,) GFP_KERNEL)
#define __alloc_objs(alloc, gfp, T, n)        ((T *)alloc(size_mul(sizeof(T), (n)), gfp))
#define __alloc_flex(alloc, gfp, T, fam, n)   ((T *)alloc(struct_size_t(T, fam, (n)), gfp))
#define kmalloc_obj(P, ...)         __alloc_objs(kmalloc, default_gfp(__VA_ARGS__), __typeof__(P), 1)
#define kmalloc_objs(P, n, ...)     __alloc_objs(kmalloc, default_gfp(__VA_ARGS__), __typeof__(P), n)
#define kmalloc_flex(P, f, n, ...)  __alloc_flex(kmalloc, default_gfp(__VA_ARGS__), __typeof__(P), f, n)
#define kzalloc_obj(P, ...)         __alloc_objs(kzalloc, default_gfp(__VA_ARGS__), __typeof__(P), 1)
#define kzalloc_objs(P, n, ...)     __alloc_objs(kzalloc, default_gfp(__VA_ARGS__), __typeof__(P), n)
#define kzalloc_flex(P, f, n, ...)  __alloc_flex(kzalloc, default_gfp(__VA_ARGS__), __typeof__(P), f, n)
#define kvzalloc_obj(P, ...)        kzalloc_obj(P, ##__VA_ARGS__)
#define kvzalloc_objs(P, n, ...)    kzalloc_objs(P, n, ##__VA_ARGS__)
#define kvmalloc_objs(P, n, ...)    kmalloc_objs(P, n, ##__VA_ARGS__)
void *kmemdup(const void *src, size_t len, gfp_t gfp);
#define kvmemdup(src, len, gfp)     kmemdup(src, len, gfp)
char *kstrdup(const char *s, gfp_t gfp);
char *kstrndup(const char *s, size_t max, gfp_t gfp);
void *kmemdup_nul(const char *s, size_t len, gfp_t gfp);

struct kmem_cache;
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
                                     unsigned int flags, void (*ctor)(void *));
void  kmem_cache_destroy(struct kmem_cache *c);
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t gfp);
#define kmem_cache_zalloc(c, gfp)   kmem_cache_alloc(c, (gfp) | __GFP_ZERO)
void  kmem_cache_free(struct kmem_cache *c, void *p);
#define SLAB_HWCACHE_ALIGN 0x2000u

/* `T *p __free(kfree) = ...;` frees p when it goes out of scope. */
DEFINE_FREE(kfree, void *, if (_T) kfree(_T))

#endif
