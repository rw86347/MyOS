#ifndef SKBUFF_H
#define SKBUFF_H

#include <stdint.h>

/*
 * A packet (Linux: struct sk_buff, "skb"), laid out as Linux drivers
 * expect it — the network stack and the Linux driver API (include/linux/
 * skbuff.h) share this one type.
 *
 * The bytes live in one block:
 *
 *   head          data          tail              end
 *     |------------|XXXXXXXXXXXX|..................|shared info|
 *      headroom    packet bytes      tailroom
 *
 * skb_reserve makes headroom (Ethernet drivers like 2 bytes, so the IP
 * header sits on a 16-byte boundary). skb_put grows the packet at the end.
 * skb_pull drops bytes from the front (the Ethernet header, once the type
 * is known). skb_push puts bytes back on the front.
 *
 * After `end` comes the "shared info": a packet may continue in pages
 * (frags) — a Wi-Fi card hands over what it received in the pages it
 * wrote, with no copy — and a clone shares the whole block with its
 * original (dataref counts them). len counts every byte, data_len only
 * those in the pages.
 *
 * Whoever holds the skb frees it with kfree_skb. A driver that is given a
 * packet to send owns it and must free it. netif_rx takes ownership too.
 * Blocks over 1 KiB are physically contiguous pages, so a driver may hand
 * skb->data to its device (DMA).
 */

struct net_device;
struct sock;
struct page;

#define MAX_SKB_FRAGS 17

typedef struct skb_frag {
    struct page *page;
    unsigned int len;
    unsigned int offset;
} skb_frag_t;

struct skb_shared_hwtstamps { int64_t hwtstamp; };

struct skb_shared_info {
    uint8_t nr_frags;
    uint8_t tx_flags;
    uint16_t gso_size, gso_segs;
    unsigned int gso_type;
    struct sk_buff *frag_list;
    struct skb_shared_hwtstamps hwtstamps;
    int dataref;                    /* skbs sharing this block (clones) */
    skb_frag_t frags[MAX_SKB_FRAGS];
};

struct sk_buff {
    /* First, as in Linux: a queue's head (struct sk_buff_head) starts with
       the same two links and stands in as the ring's empty node. */
    struct sk_buff *next, *prev;
    struct net_device *dev;
    struct sock *sk;
    int64_t tstamp;
    char cb[48] __attribute__((aligned(8)));    /* the layer that has it (mac80211's tx info) */
    unsigned int len;               /* all the packet's bytes */
    unsigned int data_len;          /* ...of which in frags */
    uint16_t mac_len, hdr_len;
    uint16_t queue_mapping;         /* which transmit queue (Wi-Fi: the access class) */
    uint8_t cloned, ip_summed, pkt_type, peeked, encapsulation, ignore_df;
    uint32_t priority;              /* 802.1d priority, 0-7 for Wi-Fi's QoS */
    uint16_t protocol;              /* Ethernet type, network byte order */
    uint16_t transport_header, network_header, mac_header;  /* offsets from head */
    uint32_t hash;
    uint16_t vlan_tci;
    uint16_t csum_start, csum_offset;
    uint32_t csum;
    uint32_t mark;
    void (*destructor)(struct sk_buff *skb);
    unsigned int truesize;          /* memory it takes, for accounting */
    uint16_t users;                 /* references; freed at 0 */
    unsigned char *head, *data, *tail, *end;
    unsigned char eth_src[6];       /* sender MAC, saved before the header is pulled */
};

#define skb_shinfo(skb) ((struct skb_shared_info *)((skb)->end))

/* Host and network byte order. x86 is little-endian; the wire is big. */
#ifndef htons
static inline uint16_t htons(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x)
{
    return (x << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }
#endif

/* `size` bytes of room (headroom + packet). NULL if memory is short. The
   flags are Linux's gfp flags (linux/gfp.h); ours never sleep. */
struct sk_buff *alloc_skb(unsigned int size, unsigned int gfp);

/* Drop one reference. At zero the packet is freed. NULL is safe. */
void kfree_skb(struct sk_buff *skb);
void consume_skb(struct sk_buff *skb);  /* same free, when we meant to */

/* How many skbs are still allocated. The self-test checks it returns to 0. */
uint32_t skb_live(void);

static inline unsigned int skb_headroom(const struct sk_buff *skb)
{
    return (unsigned int)(skb->data - skb->head);
}
static inline unsigned int skb_tailroom(const struct sk_buff *skb)
{
    return skb->data_len ? 0 : (unsigned int)(skb->end - skb->tail);
}
static inline unsigned int skb_headlen(const struct sk_buff *skb)
{
    return skb->len - skb->data_len;
}

/* Move data and tail up, before any bytes are stored. */
static inline void skb_reserve(struct sk_buff *skb, unsigned int len)
{
    skb->data += len;
    skb->tail += len;
}

/* Add len bytes at the end. Returns where they go, or NULL if they do not fit. */
void *skb_put(struct sk_buff *skb, unsigned int len);

/* Drop len bytes from the front. Returns the new data pointer, or NULL. */
void *skb_pull(struct sk_buff *skb, unsigned int len);

/* Put len bytes back on the front (they must be inside the headroom). */
void *skb_push(struct sk_buff *skb, unsigned int len);

/* For linux_skbuff.c: a block of size bytes plus the shared info (its end
   in *end), one sharer of a block less, a new descriptor to count. */
unsigned char *skb_alloc_data(unsigned int size, unsigned char **end);
void skb_release_data(struct sk_buff *skb);
void skb_count_live(void);

#endif
