/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * skbuff.c — packet buffers (step 10; Linux's layout since W1c)
 *
 * One kmalloc for the descriptor, one block for the bytes with the shared
 * info after them. A block over 1 KiB is a physically contiguous page run
 * (kmalloc_contig), so a driver can DMA it, as on Linux. kfree_skb is safe
 * from an interrupt: the counts drop with interrupts off, the frees after.
 *
 * A clone (linux_skbuff.c) is a second descriptor on the same block;
 * shinfo->dataref counts the descriptors, and the block (and the pages in
 * its frags) goes when the last one does.
 */
#include <interrupts.h>
#include <kheap.h>
#include <skbuff.h>
#include <string.h>

static uint32_t live;

void put_page(struct page *page);       /* (linux_dma.c) */

/* The bytes' block: size bytes, then the shared info. */
unsigned char *skb_alloc_data(unsigned int size, unsigned char **end)
{
    unsigned int room = (size + 15u) & ~15u;     /* the shared info stays aligned */
    size_t total = room + sizeof(struct skb_shared_info);
    unsigned char *buf = total <= 1024 ? kmalloc(total) : kmalloc_contig(total);
    if (!buf)
        return NULL;
    *end = buf + room;
    memset(*end, 0, sizeof(struct skb_shared_info));
    ((struct skb_shared_info *)*end)->dataref = 1;
    return buf;
}

struct sk_buff *alloc_skb(unsigned int size, unsigned int gfp)
{
    (void)gfp;
    struct sk_buff *skb = kmalloc(sizeof *skb);
    unsigned char *end;
    unsigned char *buf = skb ? skb_alloc_data(size ? size : 1, &end) : NULL;
    if (!skb || !buf) {
        kfree(skb);
        return NULL;
    }
    memset(skb, 0, sizeof *skb);
    skb->head = skb->data = skb->tail = buf;
    skb->end = end;
    skb->mac_header = skb->network_header = skb->transport_header = 0xFFFF;
    skb->truesize = (unsigned int)(sizeof *skb + (size_t)(end - buf));
    skb->users = 1;
    live++;
    return skb;
}

/* One sharer of the block less: the last frees it, and the frags' pages. */
void skb_release_data(struct sk_buff *skb)
{
    struct skb_shared_info *sh = skb_shinfo(skb);
    uint32_t flags = irq_save();
    int last = --sh->dataref == 0;
    irq_restore(flags);
    if (!last)
        return;
    for (int i = 0; i < sh->nr_frags; i++)
        put_page(sh->frags[i].page);
    if (sh->frag_list) {
        struct sk_buff *f = sh->frag_list;
        while (f) {
            struct sk_buff *next = f->next;
            kfree_skb(f);
            f = next;
        }
    }
    kfree(skb->head);
}

void kfree_skb(struct sk_buff *skb)
{
    if (!skb)
        return;
    uint32_t flags = irq_save();
    if (!skb->users) {
        irq_restore(flags);
        return;
    }
    skb->users--;
    int last = skb->users == 0;
    irq_restore(flags);
    if (!last)
        return;
    if (skb->destructor)
        skb->destructor(skb);
    skb_release_data(skb);
    skb->head = NULL;
    kfree(skb);
    if (live)
        live--;
}

void consume_skb(struct sk_buff *skb)
{
    kfree_skb(skb);
}

uint32_t skb_live(void)
{
    return live;
}

/* Count a descriptor that linux_skbuff.c made (a clone). */
void skb_count_live(void)
{
    live++;
}

void *skb_put(struct sk_buff *skb, unsigned int len)
{
    if (len > skb_tailroom(skb))
        return NULL;
    unsigned char *p = skb->tail;
    skb->tail += len;
    skb->len += len;
    return p;
}

void *skb_pull(struct sk_buff *skb, unsigned int len)
{
    if (len > skb_headlen(skb))
        return NULL;
    skb->len -= len;
    skb->data += len;
    return skb->data;
}

void *skb_push(struct sk_buff *skb, unsigned int len)
{
    if (len > skb_headroom(skb))
        return NULL;
    skb->data -= len;
    skb->len += len;
    return skb->data;
}
