/*
 * linux_skbuff.c — the bigger sk_buff operations of Linux's API: clones
 * (a second descriptor on the same bytes), copies, moving a packet to a
 * bigger block (pskb_expand_head), gathering page fragments into the
 * linear part, trimming, and copying bytes out across fragments.
 */
#include <linux/skbuff.h>
#include <linux/slab.h>

void kfree_skb_list(struct sk_buff *list)
{
    while (list) {
        struct sk_buff *next = list->next;
        kfree_skb(list);
        list = next;
    }
}

struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int len, gfp_t gfp)
{
    struct sk_buff *skb = alloc_skb(len + NET_SKB_PAD, gfp);
    if (skb) {
        skb_reserve(skb, NET_SKB_PAD);
        skb->dev = dev;
    }
    return skb;
}

void skb_add_rx_frag(struct sk_buff *skb, int i, struct page *page, int off, int size,
                     unsigned int truesize)
{
    skb_fill_page_desc(skb, i, page, off, size);
    skb->len += (unsigned int)size;
    skb->data_len += (unsigned int)size;
    skb->truesize += truesize;
}

/* Bytes [offset, offset+len) of the packet, wherever they are. */
int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len)
{
    u8 *dst = to;
    unsigned int head = skb_headlen(skb);
    if (offset < 0 || len < 0 || (unsigned int)(offset + len) > skb->len)
        return -EFAULT;
    if ((unsigned int)offset < head) {
        unsigned int n = min((unsigned int)len, head - (unsigned int)offset);
        memcpy(dst, skb->data + offset, n);
        dst += n;
        len -= (int)n;
        offset += (int)n;
    }
    unsigned int at = head;             /* packet offset where the next frag starts */
    const struct skb_shared_info *sh = skb_shinfo(skb);
    for (int i = 0; i < sh->nr_frags && len > 0; i++) {
        const skb_frag_t *f = &sh->frags[i];
        unsigned int size = skb_frag_size(f);
        if ((unsigned int)offset < at + size) {
            unsigned int in = (unsigned int)offset - at;
            unsigned int n = min((unsigned int)len, size - in);
            memcpy(dst, (u8 *)skb_frag_address(f) + in, n);
            dst += n;
            len -= (int)n;
            offset += (int)n;
        }
        at += size;
    }
    return len ? -EFAULT : 0;
}

/* A new block with nhead more bytes in front and ntail more behind. If the
   old block was shared (a clone), its frags gain a reference each and our
   share of it goes; if not, they simply move with us. */
int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp)
{
    (void)gfp;
    if (nhead < 0 || ntail < 0)
        return -EINVAL;
    unsigned int size = (unsigned int)(skb->end - skb->head) + (unsigned int)nhead + (unsigned int)ntail;
    unsigned char *end;
    unsigned char *data = skb_alloc_data(size, &end);
    if (!data)
        return -ENOMEM;
    struct skb_shared_info *old = skb_shinfo(skb), *sh = (struct skb_shared_info *)end;
    memcpy(data + nhead, skb->head, (size_t)(skb->tail - skb->head));
    memcpy(sh, old, sizeof *sh);
    sh->dataref = 1;
    if (old->dataref > 1) {
        for (int i = 0; i < sh->nr_frags; i++)
            get_page(sh->frags[i].page);
        for (struct sk_buff *f = sh->frag_list; f; f = f->next)
            skb_get(f);
        skb_release_data(skb);          /* only our share */
    } else {
        kfree(skb->head);               /* the frags moved with us */
    }
    long off = (data + nhead) - skb->head;
    skb->data += off;
    skb->tail += off;
    skb->head = data;
    skb->end = end;
    if (skb->mac_header != 0xFFFF)
        skb->mac_header = (u16)(skb->mac_header + nhead);
    if (skb->network_header != 0xFFFF)
        skb->network_header = (u16)(skb->network_header + nhead);
    if (skb->transport_header != 0xFFFF)
        skb->transport_header = (u16)(skb->transport_header + nhead);
    skb->cloned = 0;
    return 0;
}

/* Gather the frags' bytes into the linear part (growing it if needed) and
   let the pages go. */
int __skb_linearize(struct sk_buff *skb)
{
    unsigned int need = skb->data_len;
    if (!need)
        return 0;
    if ((unsigned int)(skb->end - skb->tail) < need || skb_cloned(skb)) {
        int grow = (int)need - (int)(skb->end - skb->tail);
        if (pskb_expand_head(skb, 0, grow > 0 ? grow : 0, GFP_ATOMIC))
            return -ENOMEM;
    }
    struct skb_shared_info *sh = skb_shinfo(skb);
    if (skb_copy_bits(skb, (int)skb_headlen(skb), skb->tail, (int)need))
        return -EFAULT;
    for (int i = 0; i < sh->nr_frags; i++)
        put_page(sh->frags[i].page);
    sh->nr_frags = 0;
    skb->tail += need;
    skb->data_len = 0;
    return 0;
}

void skb_trim(struct sk_buff *skb, unsigned int len)
{
    if (len >= skb->len)
        return;
    struct skb_shared_info *sh = skb_shinfo(skb);
    unsigned int head = skb_headlen(skb);
    if (len < head || !skb->data_len) {
        for (int i = 0; i < sh->nr_frags; i++)  /* (a clone's are shared: unshare first) */
            if (sh->dataref == 1)
                put_page(sh->frags[i].page);
        if (sh->dataref == 1)
            sh->nr_frags = 0;
        skb->data_len = 0;
        skb->tail = skb->data + len;
        skb->len = len;
        return;
    }
    unsigned int at = head;             /* keep frags up to len, shorten the last */
    int keep = 0;
    for (; keep < sh->nr_frags && at < len; keep++) {
        unsigned int size = skb_frag_size(&sh->frags[keep]);
        if (at + size > len)
            skb_frag_size_set(&sh->frags[keep], len - at);
        at += size;
    }
    for (int i = keep; i < sh->nr_frags; i++)
        put_page(sh->frags[i].page);
    sh->nr_frags = (u8)keep;
    skb->data_len = len - head;
    skb->len = len;
}

struct sk_buff *skb_clone(struct sk_buff *skb, gfp_t gfp)
{
    struct sk_buff *n = kmalloc(sizeof *n, gfp);
    if (!n)
        return NULL;
    memcpy(n, skb, sizeof *n);
    n->next = n->prev = NULL;
    n->users = 1;
    n->sk = NULL;
    n->destructor = NULL;
    n->cloned = skb->cloned = 1;
    unsigned long f = irq_save();
    skb_shinfo(skb)->dataref++;
    irq_restore(f);
    skb_count_live();
    return n;
}

static void copy_header(struct sk_buff *n, const struct sk_buff *old)
{
    long shift = (n->data - n->head) - (old->data - old->head);
    n->dev = old->dev;
    memcpy(n->cb, old->cb, sizeof n->cb);
    n->priority = old->priority;
    n->protocol = old->protocol;
    n->queue_mapping = old->queue_mapping;
    n->ip_summed = old->ip_summed;
    n->pkt_type = old->pkt_type;
    n->csum = old->csum;
    n->csum_start = old->csum_start;
    n->csum_offset = old->csum_offset;
    n->hash = old->hash;
    n->mark = old->mark;
    n->tstamp = old->tstamp;
    n->vlan_tci = old->vlan_tci;
    n->mac_len = old->mac_len;
    memcpy(n->eth_src, old->eth_src, sizeof n->eth_src);
    n->mac_header = old->mac_header == 0xFFFF ? 0xFFFF : (u16)(old->mac_header + shift);
    n->network_header = old->network_header == 0xFFFF ? 0xFFFF : (u16)(old->network_header + shift);
    n->transport_header = old->transport_header == 0xFFFF ? 0xFFFF : (u16)(old->transport_header + shift);
}

/* A linear copy with the given room in front and behind. */
struct sk_buff *skb_copy_expand(const struct sk_buff *skb, int headroom, int tailroom, gfp_t gfp)
{
    struct sk_buff *n = alloc_skb((unsigned int)(headroom + (int)skb->len + tailroom), gfp);
    if (!n)
        return NULL;
    skb_reserve(n, (unsigned int)headroom);
    skb_put(n, skb->len);
    if (skb_copy_bits(skb, 0, n->data, (int)skb->len)) {
        kfree_skb(n);
        return NULL;
    }
    copy_header(n, skb);
    return n;
}

struct sk_buff *skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
    return skb_copy_expand(skb, (int)skb_headroom(skb), (int)(skb->end - skb->tail), gfp);
}

struct sk_buff *skb_realloc_headroom(struct sk_buff *skb, unsigned int headroom)
{
    unsigned int h = max(headroom, skb_headroom(skb));
    return skb_copy_expand(skb, (int)h, 0, GFP_ATOMIC);
}
