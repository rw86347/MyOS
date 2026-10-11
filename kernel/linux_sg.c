/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * linux_sg.c — scatter lists (include/linux/scatterlist.h): counting the
 * entries, copying a buffer into them, and mapping them for a device.
 * Each walk is proportional to the list.
 */
#include <linux/devcoredump.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/scatterlist.h>

int sg_nents(struct scatterlist *sg)
{
    int n = 0;
    for (; sg; sg = sg_next(sg))
        n++;
    return n;
}

size_t sg_pcopy_from_buffer(struct scatterlist *sgl, unsigned int nents, const void *buf,
                            size_t buflen, loff_t skip)
{
    struct scatterlist *sg;
    unsigned int i;
    size_t done = 0;
    for_each_sg(sgl, sg, nents, i) {
        if (done == buflen)
            break;
        if (skip >= sg->length) {
            skip -= sg->length;
            continue;
        }
        size_t n = min_t(size_t, sg->length - (size_t)skip, buflen - done);
        memcpy((char *)sg_virt(sg) + skip, (const char *)buf + done, n);
        done += n;
        skip = 0;
    }
    return done;
}

/* Device addresses are physical addresses here (no IOMMU), so mapping is
   writing each entry's physical address down. */
int dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                    unsigned long attrs)
{
    struct scatterlist *sg;
    unsigned int i;
    (void)attrs;
    for_each_sgtable_sg(sgt, sg, i) {
        sg->dma_address = dma_map_page(dev, sg_page(sg), sg->offset, sg->length, dir);
        if (sg->dma_address == DMA_MAPPING_ERROR)
            return -EIO;
        sg->dma_length = sg->length;
    }
    sgt->nents = sgt->orig_nents;
    return 0;
}

void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                       unsigned long attrs)
{
    (void)dev; (void)dir; (void)attrs;
    sgt->nents = 0;                 /* nothing to undo: no IOMMU, no bounce buffers */
}

/* A crash dump's table: free the pages, then each array of the chain
   (an array's last entry may link to the next one). */
void _devcd_free_sgtable(struct scatterlist *table)
{
    struct scatterlist *sg;
    int i;
    for_each_sg(table, sg, sg_nents(table), i)
        if (sg_page(sg))
            __free_page(sg_page(sg));
    struct scatterlist *array = table, *it = table;
    while (!sg_is_last(it)) {
        it++;
        if (sg_is_chain(it)) {
            it = sg_chain_ptr(it);
            kfree(array);
            array = it;
        }
    }
    kfree(array);
}
