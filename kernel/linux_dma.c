/*
 * linux_dma.c — the DMA API and pages for Linux drivers.
 *
 * No IOMMU: a device address is the physical address, and x86 caches see
 * DMA, so mapping is a lookup and syncing is nothing. Memory must be
 * physically contiguous and below the device's DMA mask (some devices see
 * only 32 or 28 address bits); the page allocator hands out the lowest
 * free frames first, which suits small masks.
 */
#include <interrupts.h>
#include <kprintf.h>
#include <memlayout.h>         /* first: its PAGE_SIZE is an int, so masks reach 64-bit addresses */
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <pmm.h>
#include <string.h>

phys_addr_t virt_to_phys(const void *addr) { return V2P(addr); }
void *phys_to_virt(phys_addr_t pa) { return P2V(pa); }
phys_addr_t page_to_phys(const struct page *p) { return V2P(p); }

int dma_set_mask(struct device *dev, u64 mask)
{
    if (mask < 0xFFFFFFull)
        return -EIO;                    /* below 16 MiB: not worth supporting */
    if (dev->dma_mask)
        *dev->dma_mask = mask;
    return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
    if (mask < 0xFFFFFFull)
        return -EIO;
    dev->coherent_dma_mask = mask;
    return 0;
}

static u64 mask_of(const struct device *dev)
{
    return dev && dev->dma_mask && *dev->dma_mask ? *dev->dma_mask : ~0ull;
}

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp)
{
    (void)gfp;
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t pa = pages == 1 ? pmm_alloc() : pmm_alloc_run(pages);
    if (!pa)
        return NULL;
    u64 limit = dev && dev->coherent_dma_mask ? dev->coherent_dma_mask : ~0ull;
    if ((u64)pa + (u64)pages * PAGE_SIZE - 1 > limit) {
        for (uint32_t k = 0; k < pages; k++)
            pmm_free(pa + k * PAGE_SIZE);
        return NULL;
    }
    void *v = P2V(pa);
    memset(v, 0, (size_t)pages * PAGE_SIZE);     /* coherent memory comes zeroed */
    *handle = pa;
    return v;
}

void dma_free_coherent(struct device *dev, size_t size, void *cpu, dma_addr_t handle)
{
    (void)dev;
    (void)cpu;
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    for (uint32_t k = 0; k < pages; k++)
        pmm_free((uint32_t)handle + k * PAGE_SIZE);
}

dma_addr_t dma_map_single(struct device *dev, void *ptr, size_t size, enum dma_data_direction dir)
{
    (void)dir;
    if ((uintptr_t)ptr >= KHEAP_START || (uintptr_t)ptr < (uintptr_t)P2V(0)) {
        printk(KERN_ERR "%s: DMA from memory that isn't physically contiguous (%p)\n",
               dev ? dev_name(dev) : "dma", ptr);
        return DMA_MAPPING_ERROR;       /* vmalloc, a stack, the kernel image */
    }
    dma_addr_t a = V2P(ptr);
    if (a + size - 1 > mask_of(dev))
        return DMA_MAPPING_ERROR;       /* (no bounce buffers) */
    return a;
}

dma_addr_t dma_map_page(struct device *dev, struct page *page, size_t offset, size_t size,
                        enum dma_data_direction dir)
{
    (void)dir;
    dma_addr_t a = page_to_phys(page) + offset;
    return a + size - 1 > mask_of(dev) ? DMA_MAPPING_ERROR : a;
}

/* --- dma_pool: small coherent blocks, carved from whole pages ---------------- */

struct dma_pool {
    struct device *dev;
    size_t size;                        /* a block, rounded up to its alignment */
    struct pool_block *free;
};
struct pool_block { struct pool_block *next; dma_addr_t dma; };

struct dma_pool *dma_pool_create(const char *name, struct device *dev, size_t size,
                                 size_t align, size_t boundary)
{
    (void)name;
    (void)boundary;                     /* blocks never cross their page anyway */
    if (!align)
        align = 8;
    size = (size + align - 1) & ~(align - 1);
    if (size < sizeof(struct pool_block))
        size = sizeof(struct pool_block);
    if (size > PAGE_SIZE)
        return NULL;
    struct dma_pool *p = kzalloc(sizeof *p, GFP_KERNEL);
    if (p) {
        p->dev = dev;
        p->size = size;
    }
    return p;
}

void *dma_pool_alloc(struct dma_pool *pool, gfp_t gfp, dma_addr_t *handle)
{
    if (!pool->free) {                  /* a new page, cut into blocks */
        dma_addr_t pa;
        uint8_t *page = dma_alloc_coherent(pool->dev, PAGE_SIZE, &pa, gfp);
        if (!page)
            return NULL;
        for (size_t off = 0; off + pool->size <= PAGE_SIZE; off += pool->size) {
            struct pool_block *b = (void *)(page + off);
            b->dma = pa + off;
            b->next = pool->free;
            pool->free = b;
        }
    }
    struct pool_block *b = pool->free;
    pool->free = b->next;
    *handle = b->dma;
    memset(b, 0, pool->size);           /* (always zeroed: cheap, and safer) */
    return b;
}

void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr)
{
    struct pool_block *b = vaddr;
    b->dma = addr;
    b->next = pool->free;
    pool->free = b;
}

void dma_pool_destroy(struct dma_pool *pool)
{
    kfree(pool);                        /* (its pages stay: docs/gaps.md) */
}

/* --- pages -------------------------------------------------------------------- */

/* Order-n blocks are Linux's "compound pages": the last put_page on the
   first page frees all 2^n. Which pages start one, and their order, is
   kept in a small open hash table (they are few: receive buffers). */
#define COMPOUND_MAX 1024
static struct { uint32_t pa; uint8_t order; } compound[COMPOUND_MAX];

static int compound_slot(uint32_t pa, int insert)
{
    uint32_t h = (pa / PAGE_SIZE) % COMPOUND_MAX;
    for (uint32_t k = 0; k < COMPOUND_MAX; k++) {
        uint32_t i = (h + k) % COMPOUND_MAX;
        if (compound[i].pa == pa || (insert && !compound[i].pa))
            return (int)i;
        if (!compound[i].pa && !insert)
            return -1;
    }
    return -1;
}

struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
    uint32_t n = 1u << order;
    uint32_t pa = n == 1 ? pmm_alloc() : pmm_alloc_run(n);
    if (!pa)
        return NULL;
    if (order) {
        unsigned long f = irq_save();
        int i = compound_slot(pa, 1);
        if (i >= 0) {
            compound[i].pa = pa;
            compound[i].order = (uint8_t)order;
        }
        irq_restore(f);
        if (i < 0) {
            for (uint32_t k = 0; k < n; k++)
                pmm_free(pa + k * PAGE_SIZE);
            return NULL;
        }
    }
    void *v = P2V(pa);
    if (gfp & __GFP_ZERO)
        memset(v, 0, (size_t)n * PAGE_SIZE);
    return v;
}

/* The order of the block starting at pa (0: a single page), forgetting it
   if `take`. Removing from open addressing: re-insert what follows. */
static unsigned int compound_order(uint32_t pa, int take)
{
    unsigned long f = irq_save();
    int i = compound_slot(pa, 0);
    unsigned int order = i >= 0 ? compound[i].order : 0;
    if (i >= 0 && take) {
        compound[i].pa = 0;
        for (uint32_t k = 1; k < COMPOUND_MAX; k++) {
            uint32_t j = ((uint32_t)i + k) % COMPOUND_MAX;
            if (!compound[j].pa)
                break;
            uint32_t epa = compound[j].pa;
            uint8_t eorder = compound[j].order;
            compound[j].pa = 0;
            int n = compound_slot(epa, 1);
            compound[n].pa = epa;
            compound[n].order = eorder;
        }
    }
    irq_restore(f);
    return order;
}

void __free_pages(struct page *page, unsigned int order)
{
    uint32_t pa = (uint32_t)page_to_phys(page);
    if (order && pmm_shared(pa)) {
        pmm_free(pa);                   /* others still hold it */
        return;
    }
    if (order)
        compound_order(pa, 1);
    for (uint32_t k = 0; k < 1u << order; k++)
        pmm_free(pa + k * PAGE_SIZE);
}

unsigned long __get_free_pages(gfp_t gfp, unsigned int order)
{
    return (unsigned long)alloc_pages(gfp, order);
}

void free_pages(unsigned long addr, unsigned int order)
{
    if (addr)
        __free_pages((struct page *)addr, order);
}

void get_page(struct page *page)
{
    pmm_ref((uint32_t)page_to_phys(page));
}

void put_page(struct page *page)
{
    uint32_t pa = (uint32_t)page_to_phys(page);
    if (pmm_shared(pa)) {
        pmm_free(pa);                   /* one owner less */
        return;
    }
    unsigned int order = compound_order(pa, 1);  /* the last owner: all of it */
    for (uint32_t k = 0; k < 1u << order; k++)
        pmm_free(pa + k * PAGE_SIZE);
}
