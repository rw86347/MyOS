#ifndef LINUX_SKBUFF_H
#define LINUX_SKBUFF_H
#include <linux/in6.h>

/*
 * Linux's sk_buff API on the kernel's own packet buffer (include/skbuff.h
 * is the struct; this is what Linux drivers call on it). The small ones
 * are here, the rest in kernel/linux_skbuff.c. Queues (sk_buff_head) are
 * rings through the packets' next/prev links, with the head itself as the
 * empty node — which is why next and prev are an sk_buff's first fields.
 */
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <net/checksum.h>
#include <skbuff.h>

struct sk_buff_head {
    struct sk_buff *next, *prev;
    u32 qlen;
    spinlock_t lock;
};

#define CHECKSUM_NONE        0
#define CHECKSUM_UNNECESSARY 1
#define CHECKSUM_COMPLETE    2
#define CHECKSUM_PARTIAL     3
#define PACKET_HOST          0
#define PACKET_BROADCAST     1
#define PACKET_MULTICAST     2
#define PACKET_OTHERHOST     3
#define NET_SKB_PAD          32
#define NET_IP_ALIGN         2
#define SKB_DATA_ALIGN(x)    (((x) + 15) & ~15)
#define SKBTX_HW_TSTAMP      (1 << 0)
#define SKB_GSO_TCPV4        (1 << 0)
#define SKB_GSO_TCPV6        (1 << 4)
#define SKB_DROP_REASON_NOT_SPECIFIED 2

/* --- references and freeing ------------------------------------------------- */

static inline struct sk_buff *skb_get(struct sk_buff *skb)
{ unsigned long f = irq_save(); skb->users++; irq_restore(f); return skb; }
static inline bool skb_shared(const struct sk_buff *skb) { return skb->users != 1; }
static inline bool skb_cloned(const struct sk_buff *skb) { return skb->cloned && skb_shinfo(skb)->dataref != 1; }
#define skb_header_cloned(skb)    skb_cloned(skb)
#define dev_kfree_skb(skb)        consume_skb(skb)
#define dev_kfree_skb_any(skb)    consume_skb(skb)
#define dev_kfree_skb_irq(skb)    consume_skb(skb)
#define dev_consume_skb_any(skb)  consume_skb(skb)
#define kfree_skb_reason(skb, r)  kfree_skb(skb)
void kfree_skb_list(struct sk_buff *list);
static inline void skb_mark_not_on_list(struct sk_buff *skb) { skb->next = NULL; }
static inline void skb_orphan(struct sk_buff *skb) { if (skb->destructor) skb->destructor(skb); skb->destructor = NULL; skb->sk = NULL; }
#define skb_dst_drop(skb)         do { (void)(skb); } while (0)
#define nf_reset_ct(skb)          do { (void)(skb); } while (0)
#define skb_checksum_none_assert(skb) do { (void)(skb); } while (0)

/* --- allocation --------------------------------------------------------------- */

struct net_device;
struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int len, gfp_t gfp);
#define netdev_alloc_skb(dev, len)  __netdev_alloc_skb(dev, len, GFP_ATOMIC)
#define __dev_alloc_skb(len, gfp)   __netdev_alloc_skb(NULL, len, gfp)
#define dev_alloc_skb(len)          __netdev_alloc_skb(NULL, len, GFP_ATOMIC)
#define netdev_alloc_skb_ip_align(dev, len) ({ struct sk_buff *_s = netdev_alloc_skb(dev, (len) + NET_IP_ALIGN); \
                                               if (_s) skb_reserve(_s, NET_IP_ALIGN); _s; })

/* --- the bytes ------------------------------------------------------------------ */

static inline unsigned char *skb_tail_pointer(const struct sk_buff *skb) { return skb->tail; }
static inline unsigned char *skb_end_pointer(const struct sk_buff *skb) { return skb->end; }
static inline unsigned int skb_end_offset(const struct sk_buff *skb) { return (unsigned int)(skb->end - skb->head); }
static inline void skb_reset_tail_pointer(struct sk_buff *skb) { skb->tail = skb->data; }
static inline void skb_set_tail_pointer(struct sk_buff *skb, int off) { skb->tail = skb->data + off; }
static inline bool skb_is_nonlinear(const struct sk_buff *skb) { return skb->data_len != 0; }
static inline unsigned int skb_availroom(const struct sk_buff *skb) { return skb_tailroom(skb); }
static inline void *__skb_put(struct sk_buff *skb, unsigned int len) { return skb_put(skb, len); }
static inline void *skb_put_data(struct sk_buff *skb, const void *data, unsigned int len)
{ void *p = skb_put(skb, len); if (p) memcpy(p, data, len); return p; }
static inline void *__skb_put_data(struct sk_buff *skb, const void *data, unsigned int len)
{ return skb_put_data(skb, data, len); }
static inline void *skb_put_zero(struct sk_buff *skb, unsigned int len)
{ void *p = skb_put(skb, len); if (p) memset(p, 0, len); return p; }
static inline void *__skb_put_zero(struct sk_buff *skb, unsigned int len) { return skb_put_zero(skb, len); }
static inline void skb_put_u8(struct sk_buff *skb, u8 v) { u8 *p = skb_put(skb, 1); if (p) *p = v; }
static inline void *__skb_push(struct sk_buff *skb, unsigned int len) { return skb_push(skb, len); }
static inline void *__skb_pull(struct sk_buff *skb, unsigned int len) { return skb_pull(skb, len); }
static inline void *skb_pull_inline(struct sk_buff *skb, unsigned int len) { return skb_pull(skb, len); }
/* skb_pull_data: drop len bytes and return where they were (NULL: too short). */
static inline void *skb_pull_data(struct sk_buff *skb, size_t len)
{ void *p = skb->data; return skb_pull(skb, (unsigned int)len) ? p : NULL; }
void skb_trim(struct sk_buff *skb, unsigned int len);
#define __skb_trim(skb, len)   skb_trim(skb, len)
static inline int pskb_trim(struct sk_buff *skb, unsigned int len) { skb_trim(skb, len); return 0; }
int  skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len);
static inline void *skb_header_pointer(const struct sk_buff *skb, int offset, int len, void *buffer)
{
    if (offset >= 0 && (unsigned int)(offset + len) <= skb_headlen(skb))
        return skb->data + offset;
    return skb_copy_bits(skb, offset, buffer, len) < 0 ? NULL : buffer;
}
int  __skb_linearize(struct sk_buff *skb);
static inline int skb_linearize(struct sk_buff *skb) { return skb_is_nonlinear(skb) ? __skb_linearize(skb) : 0; }
static inline bool pskb_may_pull(struct sk_buff *skb, unsigned int len)
{ return len <= skb_headlen(skb) || (len <= skb->len && !__skb_linearize(skb)); }
#define pskb_network_may_pull(skb, len) pskb_may_pull(skb, skb_network_offset(skb) + (len))

/* --- header positions (offsets from head; 0xffff: not set) ----------------------- */

static inline unsigned char *skb_mac_header(const struct sk_buff *skb) { return skb->head + skb->mac_header; }
static inline int  skb_mac_header_was_set(const struct sk_buff *skb) { return skb->mac_header != 0xFFFF; }
static inline void skb_reset_mac_header(struct sk_buff *skb) { skb->mac_header = (u16)(skb->data - skb->head); }
static inline void skb_set_mac_header(struct sk_buff *skb, int off) { skb->mac_header = (u16)(skb->data - skb->head + off); }
static inline int  skb_mac_offset(const struct sk_buff *skb) { return (int)(skb_mac_header(skb) - skb->data); }
static inline unsigned char *skb_network_header(const struct sk_buff *skb) { return skb->head + skb->network_header; }
static inline void skb_reset_network_header(struct sk_buff *skb) { skb->network_header = (u16)(skb->data - skb->head); }
static inline void skb_set_network_header(struct sk_buff *skb, int off) { skb->network_header = (u16)(skb->data - skb->head + off); }
static inline int  skb_network_offset(const struct sk_buff *skb) { return (int)(skb_network_header(skb) - skb->data); }
static inline unsigned char *skb_transport_header(const struct sk_buff *skb) { return skb->head + skb->transport_header; }
static inline void skb_reset_transport_header(struct sk_buff *skb) { skb->transport_header = (u16)(skb->data - skb->head); }
static inline void skb_set_transport_header(struct sk_buff *skb, int off) { skb->transport_header = (u16)(skb->data - skb->head + off); }
static inline int  skb_transport_offset(const struct sk_buff *skb) { return (int)(skb_transport_header(skb) - skb->data); }
static inline u32  skb_network_header_len(const struct sk_buff *skb) { return (u32)(skb->transport_header - skb->network_header); }
static inline void skb_set_queue_mapping(struct sk_buff *skb, u16 q) { skb->queue_mapping = q; }
static inline u16  skb_get_queue_mapping(const struct sk_buff *skb) { return skb->queue_mapping; }
static inline struct skb_shared_hwtstamps *skb_hwtstamps(struct sk_buff *skb) { return &skb_shinfo(skb)->hwtstamps; }
static inline bool skb_is_gso(const struct sk_buff *skb) { return skb_shinfo(skb)->gso_size != 0; }
static inline u32  skb_get_hash(struct sk_buff *skb) { return skb->hash; }
static inline void skb_tx_timestamp(struct sk_buff *skb) { (void)skb; }

/* --- page fragments ---------------------------------------------------------- */

static inline struct page *skb_frag_page(const skb_frag_t *f) { return f->page; }
static inline unsigned int skb_frag_size(const skb_frag_t *f) { return f->len; }
static inline unsigned int skb_frag_off(const skb_frag_t *f) { return f->offset; }
static inline void skb_frag_size_set(skb_frag_t *f, unsigned int n) { f->len = n; }
static inline void skb_frag_off_set(skb_frag_t *f, unsigned int o) { f->offset = o; }
static inline void skb_frag_off_add(skb_frag_t *f, int d) { f->offset += (unsigned int)d; }
static inline void skb_frag_size_sub(skb_frag_t *f, int d) { f->len -= (unsigned int)d; }
static inline void *skb_frag_address(const skb_frag_t *f) { return (u8 *)page_address(f->page) + f->offset; }
static inline void __skb_frag_ref(skb_frag_t *f) { get_page(f->page); }
static inline void __skb_frag_unref(skb_frag_t *f) { put_page(f->page); }
static inline dma_addr_t skb_frag_dma_map(struct device *dev, const skb_frag_t *f, size_t off,
                                          size_t size, enum dma_data_direction dir)
{ return dma_map_page(dev, f->page, f->offset + off, size, dir); }
static inline void __skb_fill_page_desc(struct sk_buff *skb, int i, struct page *page, int off, int size)
{
    skb_frag_t *f = &skb_shinfo(skb)->frags[i];
    f->page = page;
    f->offset = (unsigned int)off;
    f->len = (unsigned int)size;
}
static inline void skb_fill_page_desc(struct sk_buff *skb, int i, struct page *page, int off, int size)
{ __skb_fill_page_desc(skb, i, page, off, size); skb_shinfo(skb)->nr_frags = (u8)(i + 1); }
/* A received page, appended as frag i: the skb now owns one reference. */
void skb_add_rx_frag(struct sk_buff *skb, int i, struct page *page, int off, int size,
                     unsigned int truesize);
static inline bool skb_has_frag_list(const struct sk_buff *skb) { return skb_shinfo(skb)->frag_list != NULL; }
#define skb_walk_frags(skb, it) for (it = skb_shinfo(skb)->frag_list; it; it = it->next)

/* --- copies -------------------------------------------------------------------- */

struct sk_buff *skb_clone(struct sk_buff *skb, gfp_t gfp);
struct sk_buff *skb_copy(const struct sk_buff *skb, gfp_t gfp);
struct sk_buff *skb_copy_expand(const struct sk_buff *skb, int headroom, int tailroom, gfp_t gfp);
#define pskb_copy(skb, gfp)  skb_copy(skb, gfp)
int  pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp);
static inline int skb_cow(struct sk_buff *skb, unsigned int headroom)
{
    int need = (int)headroom - (int)skb_headroom(skb);
    if (need < 0)
        need = 0;
    return need || skb_cloned(skb) ? pskb_expand_head(skb, SKB_DATA_ALIGN(need), 0, GFP_ATOMIC) : 0;
}
#define skb_cow_head(skb, h)  skb_cow(skb, h)
static inline int skb_ensure_writable(struct sk_buff *skb, unsigned int len)
{ return len > skb_headlen(skb) && __skb_linearize(skb) ? -ENOMEM : skb_cloned(skb) ? pskb_expand_head(skb, 0, 0, GFP_ATOMIC) : 0; }
struct sk_buff *skb_realloc_headroom(struct sk_buff *skb, unsigned int headroom);
static inline struct sk_buff *skb_unshare(struct sk_buff *skb, gfp_t gfp)
{
    if (!skb_cloned(skb))
        return skb;
    struct sk_buff *n = skb_copy(skb, gfp);
    kfree_skb(skb);
    return n;
}
/* TSO is never asked for (our stack sends whole frames). */
static inline struct sk_buff *skb_gso_segment(struct sk_buff *skb, u64 features)
{ (void)skb; (void)features; return (struct sk_buff *)(long)-95; }
struct scatterlist;
static inline int skb_to_sgvec(struct sk_buff *skb, struct scatterlist *sg, int off, int len)
{ (void)skb; (void)sg; (void)off; (void)len; return -ENOMEM; }

/* --- queues ------------------------------------------------------------------------- */

static inline void __skb_queue_head_init(struct sk_buff_head *l)
{ l->next = l->prev = (struct sk_buff *)l; l->qlen = 0; }
static inline void skb_queue_head_init(struct sk_buff_head *l) { spin_lock_init(&l->lock); __skb_queue_head_init(l); }
static inline bool skb_queue_empty(const struct sk_buff_head *l) { return l->next == (const struct sk_buff *)l; }
#define skb_queue_empty_lockless(l) skb_queue_empty(l)
static inline u32 skb_queue_len(const struct sk_buff_head *l) { return l->qlen; }
#define skb_queue_len_lockless(l) skb_queue_len(l)
static inline struct sk_buff *skb_peek(const struct sk_buff_head *l)
{ return skb_queue_empty(l) ? NULL : l->next; }
static inline struct sk_buff *skb_peek_tail(const struct sk_buff_head *l)
{ return skb_queue_empty(l) ? NULL : l->prev; }
static inline struct sk_buff *skb_peek_next(struct sk_buff *skb, const struct sk_buff_head *l)
{ return skb->next == (const struct sk_buff *)l ? NULL : skb->next; }
static inline bool skb_queue_is_last(const struct sk_buff_head *l, const struct sk_buff *skb)
{ return skb->next == (const struct sk_buff *)l; }
static inline bool skb_queue_is_first(const struct sk_buff_head *l, const struct sk_buff *skb)
{ return skb->prev == (const struct sk_buff *)l; }
/* The two links every queue member and the head share. prev or next may
   be the head, which is only this big: link through this view of it. */
struct sk_buff_list { struct sk_buff *next, *prev; };
static inline void __skb_insert(struct sk_buff *n, struct sk_buff *prev, struct sk_buff *next,
                                struct sk_buff_head *l)
{
    n->next = next;
    n->prev = prev;
    ((struct sk_buff_list *)next)->prev = n;
    ((struct sk_buff_list *)prev)->next = n;
    l->qlen++;
}
static inline void __skb_queue_tail(struct sk_buff_head *l, struct sk_buff *n)
{ __skb_insert(n, l->prev, (struct sk_buff *)l, l); }
static inline void __skb_queue_head(struct sk_buff_head *l, struct sk_buff *n)
{ __skb_insert(n, (struct sk_buff *)l, l->next, l); }
static inline void __skb_queue_after(struct sk_buff_head *l, struct sk_buff *prev, struct sk_buff *n)
{ __skb_insert(n, prev, prev->next, l); }
static inline void __skb_queue_before(struct sk_buff_head *l, struct sk_buff *next, struct sk_buff *n)
{ __skb_insert(n, next->prev, next, l); }
static inline void __skb_unlink(struct sk_buff *skb, struct sk_buff_head *l)
{
    l->qlen--;
    ((struct sk_buff_list *)skb->next)->prev = skb->prev;
    ((struct sk_buff_list *)skb->prev)->next = skb->next;
    skb->next = skb->prev = NULL;
}
static inline struct sk_buff *__skb_dequeue(struct sk_buff_head *l)
{ struct sk_buff *s = skb_peek(l); if (s) __skb_unlink(s, l); return s; }
static inline struct sk_buff *__skb_dequeue_tail(struct sk_buff_head *l)
{ struct sk_buff *s = skb_peek_tail(l); if (s) __skb_unlink(s, l); return s; }
#define __LOCKED_Q(expr) ({ unsigned long _f = irq_save(); __typeof__(expr) _r = (expr); irq_restore(_f); _r; })
#define __LOCKED_V(stmt) do { unsigned long _f = irq_save(); stmt; irq_restore(_f); } while (0)
#define skb_queue_tail(l, n)  __LOCKED_V(__skb_queue_tail(l, n))
#define skb_queue_head(l, n)  __LOCKED_V(__skb_queue_head(l, n))
#define skb_unlink(s, l)      __LOCKED_V(__skb_unlink(s, l))
#define skb_dequeue(l)        __LOCKED_Q(__skb_dequeue(l))
#define skb_dequeue_tail(l)   __LOCKED_Q(__skb_dequeue_tail(l))
static inline void __skb_queue_purge(struct sk_buff_head *l)
{ struct sk_buff *s; while ((s = __skb_dequeue(l)) != NULL) kfree_skb(s); }
static inline void skb_queue_purge(struct sk_buff_head *l)
{ struct sk_buff *s; while ((s = skb_dequeue(l)) != NULL) kfree_skb(s); }
#define skb_queue_purge_reason(l, r) skb_queue_purge(l)
static inline void __skb_queue_splice(const struct sk_buff_head *list, struct sk_buff *prev, struct sk_buff *next)
{
    struct sk_buff *first = list->next, *last = list->prev;
    ((struct sk_buff_list *)first)->prev = prev;
    ((struct sk_buff_list *)prev)->next = first;
    ((struct sk_buff_list *)last)->next = next;
    ((struct sk_buff_list *)next)->prev = last;
}
static inline void skb_queue_splice_init(struct sk_buff_head *list, struct sk_buff_head *head)
{
    if (!skb_queue_empty(list)) {
        __skb_queue_splice(list, (struct sk_buff *)head, head->next);
        head->qlen += list->qlen;
        __skb_queue_head_init(list);
    }
}
static inline void skb_queue_splice_tail_init(struct sk_buff_head *list, struct sk_buff_head *head)
{
    if (!skb_queue_empty(list)) {
        __skb_queue_splice(list, head->prev, (struct sk_buff *)head);
        head->qlen += list->qlen;
        __skb_queue_head_init(list);
    }
}
#define skb_queue_splice(list, head)       do { if (!skb_queue_empty(list)) { \
        __skb_queue_splice(list, (struct sk_buff *)(head), (head)->next); (head)->qlen += (list)->qlen; } } while (0)
#define skb_queue_splice_tail(list, head)  do { if (!skb_queue_empty(list)) { \
        __skb_queue_splice(list, (head)->prev, (struct sk_buff *)(head)); (head)->qlen += (list)->qlen; } } while (0)
#define skb_queue_walk(q, skb) \
    for (skb = (q)->next; skb != (struct sk_buff *)(q); skb = skb->next)
#define skb_queue_walk_safe(q, skb, tmp) \
    for (skb = (q)->next, tmp = skb->next; skb != (struct sk_buff *)(q); skb = tmp, tmp = skb->next)
#define skb_queue_walk_from(q, skb) \
    for (; skb != (struct sk_buff *)(q); skb = skb->next)
#define skb_queue_reverse_walk(q, skb) \
    for (skb = (q)->prev; skb != (struct sk_buff *)(q); skb = skb->prev)
#define skb_list_walk_safe(first, skb, next_skb) \
    for ((skb) = (first), (next_skb) = (skb) ? (skb)->next : NULL; (skb); \
         (skb) = (next_skb), (next_skb) = (skb) ? (skb)->next : NULL)
static inline void skb_list_del_init(struct sk_buff *skb) { skb->next = skb->prev = NULL; }

/* After pushing len bytes at start: keep a "complete" checksum right. */
static inline void skb_postpush_rcsum(struct sk_buff *skb, const void *start, unsigned int len)
{
    if (skb->ip_summed == CHECKSUM_COMPLETE)
        skb->csum = csum_partial(start, (int)len, skb->csum);
}

#endif
