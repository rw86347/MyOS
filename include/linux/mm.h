/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_MM_H
#define LINUX_MM_H

/*
 * Pages. A `struct page *` is, here, simply the page's address in the
 * kernel's direct map of RAM (so page_address is a cast and nothing has
 * to be looked up); its owners are the page allocator's share counts
 * (mm/pmm.c): get_page adds one, put_page drops one, the last frees it.
 * Order-n allocations are 2^n contiguous pages; free them with
 * __free_pages(page, n). (kernel/linux_dma.c)
 */
#include <linux/bitops.h>
#include <linux/gfp.h>
#include <linux/types.h>

#define PAGE_SHIFT 12
#ifndef PAGE_SIZE
#define PAGE_SIZE (1UL << PAGE_SHIFT)   /* unsigned long, as drivers expect (%lu) */
#endif
#define PAGE_MASK  (~(unsigned long)(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & PAGE_MASK)

struct page;
struct page *alloc_pages(gfp_t gfp, unsigned int order);
#define alloc_page(gfp)              alloc_pages(gfp, 0)
#define dev_alloc_pages(order)       alloc_pages(GFP_ATOMIC, order)
#define dev_alloc_page()             alloc_pages(GFP_ATOMIC, 0)
void __free_pages(struct page *page, unsigned int order);
#define __free_page(p)               __free_pages(p, 0)
unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
#define __get_free_page(gfp)         __get_free_pages(gfp, 0)
#define get_zeroed_page(gfp)         __get_free_pages((gfp) | __GFP_ZERO, 0)
void free_pages(unsigned long addr, unsigned int order);
#define free_page(addr)              free_pages(addr, 0)
void get_page(struct page *page);
void put_page(struct page *page);
static inline void *page_address(const struct page *p) { return (void *)p; }
static inline struct page *virt_to_page(const void *addr)
{ return (struct page *)((unsigned long)addr & PAGE_MASK); }
phys_addr_t page_to_phys(const struct page *p);
phys_addr_t virt_to_phys(const void *addr);
void *phys_to_virt(phys_addr_t pa);
static inline int page_count(const struct page *p) { (void)p; return 1; }
#define page_ref_count(p)  page_count(p)
#define get_order(size)    ((size) <= PAGE_SIZE ? 0 : (int)(fls64((u64)(size) - 1) - PAGE_SHIFT))

#endif
