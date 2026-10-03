/*
 * packet.c — build a frame, give it to the stack, answer what comes back.
 */

#include <netdevice.h>
#include <stdint.h>
#include <string.h>

#include "packet.h"
#include "stubs.h"

const uint8_t peer_mac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };

/* A different MAC for the attacker, so a test can tell whose address ended
   up in the ARP cache rather than merely that something did. */
const uint8_t evil_mac[6] = { 0x00, 0xBA, 0xDB, 0xAD, 0xBA, 0xD0 };

uint16_t pk_csum(const void *d, uint32_t n)
{
    return ip_csum_finish(ip_csum_add(0, d, n));
}

void pk_feed(uint16_t type, const uint8_t *b, uint32_t n, const uint8_t mac[6])
{
    int (*fn)(struct sk_buff *) = fuzz_handler(type);
    if (!fn)
        return;
    struct sk_buff *skb = alloc_skb(n + 64);
    if (!skb)
        return;
    skb_reserve(skb, 16);
    unsigned char *p = skb_put(skb, n);
    if (!p) {
        kfree_skb(skb);
        return;
    }
    if (n)
        memcpy(p, b, n);
    skb->protocol = htons(type);
    skb->dev = fuzz_dev();
    memcpy(skb->eth_src, mac, 6);
    fn(skb);                        /* takes ownership */
}

uint32_t pk_ip(uint8_t *out, uint32_t cap, uint8_t proto,
               const uint8_t *body, uint32_t n)
{
    if (cap < IP_HLEN + n)
        return 0;
    memset(out, 0, IP_HLEN);
    out[IP_VER_IHL] = 0x45;
    wr16(out + IP_TOT_LEN, (uint16_t)(IP_HLEN + n));
    wr16(out + IP_ID, 0x1234);
    out[IP_TTL] = 64;
    out[IP_PROTO] = proto;
    wr32(out + IP_SADDR, PEER_IP);
    wr32(out + IP_DADDR, OUR_IP);
    if (n)
        memcpy(out + IP_HLEN, body, n);
    wr16(out + IP_CHECK, pk_csum(out, IP_HLEN));
    return IP_HLEN + n;
}

void pk_ip_set_dst(uint8_t *pkt, uint32_t dst)
{
    wr32(pkt + IP_DADDR, dst);
    wr16(pkt + IP_CHECK, 0);
    wr16(pkt + IP_CHECK, pk_csum(pkt, IP_HLEN));
}

void pk_ip_set_src(uint8_t *pkt, uint32_t src)
{
    wr32(pkt + IP_SADDR, src);
    wr16(pkt + IP_CHECK, 0);
    wr16(pkt + IP_CHECK, pk_csum(pkt, IP_HLEN));
}

void pk_arp(uint8_t out[ARP_LEN], uint16_t oper, const uint8_t sha[6],
            uint32_t spa, uint32_t tpa)
{
    memset(out, 0, ARP_LEN);
    wr16(out, 1);                   /* Ethernet */
    wr16(out + 2, 0x0800);          /* IPv4 */
    out[4] = 6;
    out[5] = 4;
    wr16(out + ARP_OPER, oper);
    memcpy(out + ARP_SHA, sha, 6);
    wr32(out + ARP_SPA, spa);
    wr32(out + ARP_TPA, tpa);
}

static void (*ip_sink)(const uint8_t *ip, uint32_t len);

void pk_set_ip_sink(void (*fn)(const uint8_t *ip, uint32_t len))
{
    ip_sink = fn;
}

/*
 * Answer an ARP request the stack made. Without this nothing addressed to a
 * neighbour ever leaves: ip_send parks the frame and sends a request, and
 * the frame only goes out when the reply arrives.
 */
static void answer_arp(const uint8_t *arp)
{
    uint8_t reply[ARP_LEN];
    pk_arp(reply, 2 /* reply */, peer_mac, rd32(arp + ARP_TPA), OUR_IP);
    pk_feed(ETH_P_ARP, reply, sizeof reply, peer_mac);
}

void pk_pump(void)
{
    for (int pass = 0; pass < 8; pass++) {
        struct fuzz_frame f;
        int any = 0;
        while (fuzz_take(&f)) {
            any = 1;
            if (f.len < ETH_HLEN)
                continue;
            uint16_t type = rd16(f.b + ETH_TYPE);
            const uint8_t *body = f.b + ETH_HLEN;
            uint32_t blen = f.len - ETH_HLEN;
            if (type == ETH_P_ARP && blen >= ARP_LEN && rd16(body + ARP_OPER) == 1)
                answer_arp(body);
            else if (type == ETH_P_IP && blen >= IP_HLEN && ip_sink)
                ip_sink(body, blen);
        }
        if (!any)
            break;
    }
}

void pk_prime_arp(void)
{
    static const uint8_t pad[8] = { 0 };
    void (*save)(const uint8_t *, uint32_t) = ip_sink;
    ip_sink = NULL;
    ip_send(PEER_IP, IPPROTO_UDP, pad, sizeof pad);
    pk_pump();
    ip_send(IPV4_DNS, IPPROTO_UDP, pad, sizeof pad);
    pk_pump();
    fuzz_sent_reset();
    ip_sink = save;
}
