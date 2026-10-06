#ifndef LINUX_SCATTERLIST_H
#define LINUX_SCATTERLIST_H
/*
 * Scatter lists: one buffer made of pieces in different pages. Each entry
 * names a page, an offset and a length, plus the address the device uses
 * once mapped. Arrays of entries can be chained: the last entry of one
 * array then points at the next array instead of a page.
 *
 * As in Linux, the low two bits of page_link say which: bit 0 "this is
 * a chain link", bit 1 "this is the last entry". (A struct page * is a
 * direct-map address here, so its low bits are free.)
 */
#include <linux/mm.h>
#include <linux/types.h>
#include <string.h>

struct scatterlist {
    unsigned long page_link;
    unsigned int offset;
    unsigned int length;
    dma_addr_t dma_address;
    unsigned int dma_length;
};

struct sg_table {
    struct scatterlist *sgl;
    unsigned int nents;             /* mapped entries */
    unsigned int orig_nents;        /* entries in the list */
};

#define SG_CHAIN            0x01ul
#define SG_END              0x02ul
#define SG_PAGE_LINK_MASK   (SG_CHAIN | SG_END)

static inline bool sg_is_chain(const struct scatterlist *sg) { return sg->page_link & SG_CHAIN; }
static inline bool sg_is_last(const struct scatterlist *sg) { return sg->page_link & SG_END; }
static inline struct scatterlist *sg_chain_ptr(const struct scatterlist *sg)
{
    return (struct scatterlist *)(sg->page_link & ~SG_PAGE_LINK_MASK);
}
static inline struct page *sg_page(const struct scatterlist *sg)
{
    return (struct page *)(sg->page_link & ~SG_PAGE_LINK_MASK);
}
static inline void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len,
                               unsigned int offset)
{
    sg->page_link = (sg->page_link & SG_END) | (unsigned long)page;
    sg->offset = offset;
    sg->length = len;
}
static inline void *sg_virt(const struct scatterlist *sg)
{
    return (char *)page_address(sg_page(sg)) + sg->offset;
}
static inline void sg_mark_end(struct scatterlist *sg)
{
    sg->page_link = (sg->page_link | SG_END) & ~SG_CHAIN;
}
static inline void sg_init_table(struct scatterlist *sgl, unsigned int nents)
{
    memset(sgl, 0, sizeof(*sgl) * nents);
    sg_mark_end(&sgl[nents - 1]);
}
/* Make the last entry of prv (prv_nents long) a link to sgl. */
static inline void sg_chain(struct scatterlist *prv, unsigned int prv_nents, struct scatterlist *sgl)
{
    struct scatterlist *link = &prv[prv_nents - 1];
    link->offset = 0;
    link->length = 0;
    link->page_link = (unsigned long)sgl | SG_CHAIN;
}
static inline struct scatterlist *sg_next(struct scatterlist *sg)
{
    if (sg_is_last(sg))
        return NULL;
    sg++;
    return sg_is_chain(sg) ? sg_chain_ptr(sg) : sg;
}

#define for_each_sg(sglist, sg, nr, i) \
    for ((i) = 0, (sg) = (sglist); (i) < (nr); (i)++, (sg) = sg_next(sg))
#define for_each_sgtable_sg(sgt, sg, i)      for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i)  for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#define sg_dma_address(sg)  ((sg)->dma_address)
#define sg_dma_len(sg)      ((sg)->dma_length)

int sg_nents(struct scatterlist *sg);
/* Copy buflen bytes into the list's pages, starting skip bytes in. */
size_t sg_pcopy_from_buffer(struct scatterlist *sgl, unsigned int nents, const void *buf,
                            size_t buflen, loff_t skip);
#endif
