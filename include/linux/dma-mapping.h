/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_DMA_MAPPING_H
#define LINUX_DMA_MAPPING_H

/*
 * DMA: memory a device reads and writes by itself (kernel/linux_dma.c).
 * MyOS has no IOMMU, so a device address is the physical address. x86
 * caches watch the bus, so syncs are only compiler barriers. What matters
 * is that the memory is physically contiguous: dma_alloc_coherent hands
 * out whole page runs, and the shim's kmalloc is contiguous too (as
 * Linux's is), so dma_map_single works on it. vmalloc memory is not —
 * mapping it is an error, as on Linux.
 */
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/scatterlist.h>

enum dma_data_direction { DMA_BIDIRECTIONAL = 0, DMA_TO_DEVICE = 1, DMA_FROM_DEVICE = 2, DMA_NONE = 3 };
#define DMA_BIT_MASK(n)     (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR   (~(dma_addr_t)0)
#define DMA_ATTR_SKIP_CPU_SYNC (1UL << 5)

int  dma_set_mask(struct device *dev, u64 mask);
int  dma_set_coherent_mask(struct device *dev, u64 mask);
static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{ int e = dma_set_mask(dev, mask); return e ? e : dma_set_coherent_mask(dev, mask); }
void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp);
void  dma_free_coherent(struct device *dev, size_t size, void *cpu, dma_addr_t handle);
#define dmam_alloc_coherent(d, s, h, g) dma_alloc_coherent(d, s, h, g)
dma_addr_t dma_map_single(struct device *dev, void *ptr, size_t size, enum dma_data_direction dir);
#define dma_map_single_attrs(d, p, s, dir, a) dma_map_single(d, p, s, dir)
static inline void dma_unmap_single(struct device *d, dma_addr_t a, size_t s, enum dma_data_direction dir)
{ (void)d; (void)a; (void)s; (void)dir; }
dma_addr_t dma_map_page(struct device *dev, struct page *page, size_t offset, size_t size,
                        enum dma_data_direction dir);
#define dma_map_page_attrs(d, p, o, s, dir, a) dma_map_page(d, p, o, s, dir)
static inline void dma_unmap_page(struct device *d, dma_addr_t a, size_t s, enum dma_data_direction dir)
{ (void)d; (void)a; (void)s; (void)dir; }
#define dma_unmap_page_attrs(d, a, s, dir, at) dma_unmap_page(d, a, s, dir)
static inline int dma_mapping_error(struct device *d, dma_addr_t a) { (void)d; return a == DMA_MAPPING_ERROR; }
#define dma_sync_single_for_cpu(d, a, s, dir)    __asm__ volatile ("" ::: "memory")
#define dma_sync_single_for_device(d, a, s, dir) __asm__ volatile ("" ::: "memory")
#define dma_sync_single_range_for_cpu(d, a, o, s, dir)    __asm__ volatile ("" ::: "memory")
#define dma_sync_single_range_for_device(d, a, o, s, dir) __asm__ volatile ("" ::: "memory")
#define dma_unmap_addr(p, n)           ((p)->n)
#define dma_unmap_addr_set(p, n, v)    ((p)->n = (v))
#define dma_unmap_len(p, n)            ((p)->n)
#define dma_unmap_len_set(p, n, v)     ((p)->n = (v))
#define DEFINE_DMA_UNMAP_ADDR(n)       dma_addr_t n
#define DEFINE_DMA_UNMAP_LEN(n)        u32 n
static inline size_t dma_max_mapping_size(struct device *d) { (void)d; return 1u << 20; }

/* A dma_pool: many small coherent blocks of one size. */
struct dma_pool;
struct dma_pool *dma_pool_create(const char *name, struct device *dev, size_t size,
                                 size_t align, size_t boundary);
void  dma_pool_destroy(struct dma_pool *pool);
void *dma_pool_alloc(struct dma_pool *pool, gfp_t gfp, dma_addr_t *handle);
#define dma_pool_zalloc(p, g, h) dma_pool_alloc(p, (g) | 0x0100u /* __GFP_ZERO */, h)
void  dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr);
#define dmam_pool_create(n, d, s, a, b) dma_pool_create(n, d, s, a, b)

/* Map / unmap every entry of a scatter list for the device (linux_sg.c). */
int dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                    unsigned long attrs);
void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                       unsigned long attrs);

#endif
