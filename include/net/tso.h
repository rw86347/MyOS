#ifndef NET_TSO_H
#define NET_TSO_H
/*
 * Linux's TCP segmentation helpers, for drivers that split an oversized
 * TCP packet themselves. Our stack never hands a driver such a packet
 * (gso_size is always 0), so these paths compile and are never taken;
 * the helpers say so if they ever were.
 */
#include <linux/kernel.h>
#include <linux/skbuff.h>
struct tso_t {
    int next_frag_idx;
    int size;
    void *data;
    u16 ip_id;
    u8 tlen;
    bool ipv6;
    u32 tcp_seq;
};
#define TSO_HEADER_SIZE 256
static inline int tso_start(struct sk_buff *skb, struct tso_t *tso)
{ (void)skb; WARN_ON_ONCE(1); memset(tso, 0, sizeof *tso); return 0; }
static inline void tso_build_hdr(const struct sk_buff *skb, char *hdr, struct tso_t *tso, int size, bool last)
{ (void)skb; (void)hdr; (void)tso; (void)size; (void)last; WARN_ON_ONCE(1); }
static inline void tso_build_data(const struct sk_buff *skb, struct tso_t *tso, int size)
{ (void)skb; (void)tso; (void)size; WARN_ON_ONCE(1); }
static inline int tso_count_descs(const struct sk_buff *skb) { (void)skb; return 0; }
#endif
