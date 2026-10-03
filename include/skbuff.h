#ifndef SKBUFF_H
#define SKBUFF_H

#include <stdint.h>

/*
 * A packet (Linux: struct sk_buff, "skb").
 *
 * The bytes live in one block:
 *
 *   head          data          tail              end
 *     |------------|XXXXXXXXXXXX|..................|
 *      headroom    packet bytes      tailroom
 *
 * skb_reserve makes headroom (Ethernet drivers like 2 bytes, so the IP
 * header sits on a 16-byte boundary). skb_put grows the packet at the end.
 * skb_pull drops bytes from the front (the Ethernet header, once the type
 * is known). skb_push puts bytes back on the front.
 *
 * Whoever holds the skb frees it with kfree_skb. A driver that is given a
 * packet to send owns it and must free it. netif_rx takes ownership too.
 */

struct net_device;

struct sk_buff {
    struct sk_buff *next;           /* the receive queue */
    struct net_device *dev;         /* the device it came from, or goes to */
    unsigned char *head, *data, *tail, *end;
    unsigned int len;               /* bytes between data and tail */
    uint16_t protocol;              /* Ethernet type, network byte order */
    uint16_t users;                 /* references; freed at 0 */
    unsigned char eth_src[6];       /* sender MAC, saved before the header is pulled */
};

/* Host and network byte order. i386 is little-endian; the wire is big. */
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

/* `size` bytes of room (headroom + packet). NULL if memory is short. */
struct sk_buff *alloc_skb(unsigned int size);

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
    return (unsigned int)(skb->end - skb->tail);
}

/* Move data and tail up, before any bytes are stored. */
static inline void skb_reserve(struct sk_buff *skb, unsigned int len)
{
    skb->data += len;
    skb->tail += len;
}

/* Add len bytes at the end. Returns where they go, or NULL if they do not fit. */
unsigned char *skb_put(struct sk_buff *skb, unsigned int len);

/* Drop len bytes from the front. Returns the new data pointer, or NULL. */
unsigned char *skb_pull(struct sk_buff *skb, unsigned int len);

/* Put len bytes back on the front (they must be inside the headroom). */
unsigned char *skb_push(struct sk_buff *skb, unsigned int len);

#endif
