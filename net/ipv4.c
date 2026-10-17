/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * ipv4.c — ARP, ICMP, and the IP header check (step 10)
 *
 * The address, netmask and gateway come from DHCP at boot (net/dhcp.c) and
 * live in ipv4_addr / ipv4_mask / ipv4_gw. They are zero until a lease
 * arrives, so every path in here has to cope with having no address -- which
 * mostly means refusing to do anything, the one exception being the BOOTP
 * broadcast the DHCP client sends to ask.
 * The parser trusts lengths only after checking them against the bytes we
 * actually have. IP options and fragments are dropped. UDP and TCP sit on
 * top of ip_send / the demux at the bottom of on_ip.
 *
 * ip_send never blocks. If the next hop's MAC is unknown, the finished
 * frame waits in a small fixed queue while an ARP request goes out, and
 * the reply sends it. That matters for more than speed: a send that slept
 * here would be re-entered from the receive path, and a caller's buffer
 * (tcp.c builds segments in one static buffer) could be overwritten under
 * it before the bytes reached the card.
 *
 * The neighbour cache only ever learns from a reply to a request of our
 * own. Unsolicited replies and other stations' requests are how ARP caches
 * get poisoned, so they are answered where appropriate but never recorded.
 */
#include <dhcp.h>
#include <errno.h>
#include <interrupts.h>
#include <ipv4.h>
#include <kprintf.h>
#include <netdevice.h>
#include <random.h>
#include <socket.h>
#include <string.h>
#include <tcp.h>
#include <thread.h>
#include <timer.h>
#include <udp.h>

/* Set once by the DHCP client before any socket exists; see include/ipv4.h. */
uint32_t ipv4_addr, ipv4_mask = IPV4_MASK_STATIC, ipv4_gw, ipv4_dns;

#define OUR_IP    ipv4_addr
#define OUR_MASK  ipv4_mask
#define OUR_GW    ipv4_gw

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t oper;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} __attribute__((packed));

struct iphdr {
    uint8_t ver_ihl, tos;
    uint16_t tot_len, id, frag;
    uint8_t ttl, protocol;
    uint16_t check;
    uint32_t saddr, daddr;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t type, code;
    uint16_t check;
    uint16_t id, seq;
} __attribute__((packed));

#define ARP_REQUEST 1
#define ARP_REPLY   2
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO       8

#define NEIGH_N      8
#define NEIGH_TTL_MS 60000          /* ask again about a host after a minute */

struct neigh {
    uint32_t ip;
    uint8_t mac[6];
    uint32_t used_at;               /* for least-recently-used eviction */
    uint32_t expires;               /* 0: slot free */
};

static struct neigh table[NEIGH_N];
static struct net_device *eth;

struct net_device *ipv4_dev(void)
{
    return eth;
}
static volatile int ping_got;
static uint16_t ping_seq;
static uint32_t ping_dst;

static int past(uint32_t deadline, uint32_t now)
{
    return (int32_t)(now - deadline) >= 0;
}

uint32_t ip_csum_add(uint32_t sum, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)p[0] << 8 | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t ip_csum_finish(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t checksum(const void *data, uint32_t len)
{
    return ip_csum_finish(ip_csum_add(0, data, len));
}

static void wr_be32(uint8_t a[4], uint32_t ip);

uint16_t ip_transport_csum(uint32_t src, uint32_t dst, uint8_t proto,
                           const void *seg, uint32_t len)
{
    uint8_t ph[12];
    wr_be32(ph, src);
    wr_be32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = proto;
    ph[10] = (uint8_t)(len >> 8);
    ph[11] = (uint8_t)len;
    uint32_t sum = ip_csum_add(0, ph, 12);
    sum = ip_csum_add(sum, seg, len);
    return ip_csum_finish(sum);
}

static uint32_t rd_be32(const uint8_t a[4])
{
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
           ((uint32_t)a[2] << 8) | a[3];
}

static void wr_be32(uint8_t a[4], uint32_t ip)
{
    a[0] = (uint8_t)(ip >> 24);
    a[1] = (uint8_t)(ip >> 16);
    a[2] = (uint8_t)(ip >> 8);
    a[3] = (uint8_t)ip;
}

static int eth_send(const uint8_t dst[6], uint16_t proto, const void *payload, uint32_t len)
{
    if (!eth || len > ETH_DATA_LEN)
        return -EINVAL;
    struct sk_buff *skb = alloc_skb(2 + ETH_HLEN + len, 0);
    if (!skb)
        return -ENOMEM;
    skb_reserve(skb, 2);
    unsigned char *p = skb_put(skb, ETH_HLEN + len);
    if (!p) {
        kfree_skb(skb);
        return -ENOMEM;
    }
    memcpy(p, dst, 6);
    memcpy(p + 6, eth->dev_addr, 6);
    p[12] = (uint8_t)(proto >> 8);
    p[13] = (uint8_t)proto;
    memcpy(p + ETH_HLEN, payload, len);
    skb->dev = eth;
    int rc = dev_queue_xmit(skb);
    if (rc == -EAGAIN)
        kfree_skb(skb);             /* the driver did not take it */
    return rc;
}

/* A full table drops the least recently used entry, not the first one: an
   attacker cannot aim the eviction at the gateway, which is in constant use. */
static void remember(uint32_t ip, const uint8_t mac[6])
{
    uint32_t now = timer_ms();
    int slot = -1;
    for (int i = 0; i < NEIGH_N; i++) {
        if (table[i].expires && table[i].ip == ip) {
            slot = i;
            break;
        }
        if (!table[i].expires && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < NEIGH_N; i++)
            if ((int32_t)(table[i].used_at - table[slot].used_at) < 0)
                slot = i;
    }
    table[slot].ip = ip;
    memcpy(table[slot].mac, mac, 6);
    table[slot].used_at = now;
    table[slot].expires = (now + NEIGH_TTL_MS) | 1;     /* never 0: 0 is free */
}

static int lookup(uint32_t ip, uint8_t mac[6])
{
    uint32_t now = timer_ms();
    for (int i = 0; i < NEIGH_N; i++) {
        if (!table[i].expires || table[i].ip != ip)
            continue;
        if (past(table[i].expires, now)) {
            table[i].expires = 0;       /* stale: ask again */
            return -1;
        }
        memcpy(mac, table[i].mac, 6);
        table[i].used_at = now;
        return 0;
    }
    return -1;
}

/* A station we may talk to: not zero, not a broadcast, not a multicast. */
static int ip_ok(uint32_t ip)
{
    return ip && ip != 0xFFFFFFFFu && (ip >> 28) != 0xE;
}

/* Whether an address is a believable *sender*. A packet arrives with whatever
   source its sender cared to write, and we hand that address to the code that
   decides where a reply goes (RFC 1122 3.2.1.3 says to discard these). Three
   of them are worth naming:
     - a broadcast or multicast source would aim our reply at a whole segment;
     - 127/8 cannot reach us from a wire, so a frame carrying it is forged;
     - our own address claimed by someone else is a spoof, and it is also the
       shape of the old "land" attack: a packet from us, to us.
   Dropping them here rather than in the echo path covers ICMP, TCP, UDP and
   raw sockets at once, and keeps an attacker from choosing which per-
   destination IP-ID counter a reply of ours reveals. */
static int ip_source_ok(uint32_t ip)
{
    return ip_ok(ip) && (ip >> 24) != 127 && ip != OUR_IP &&
           ip != (OUR_IP | ~OUR_MASK);          /* the subnet broadcast */
}

static int mac_ok(const uint8_t mac[6])
{
    if (mac[0] & 1)                     /* multicast or broadcast */
        return 0;
    for (int i = 0; i < 6; i++)
        if (mac[i])
            return 1;
    return 0;                           /* all zero */
}

static void arp_send(uint16_t oper, const uint8_t tha[6], uint32_t tpa)
{
    struct arp_pkt a;
    memset(&a, 0, sizeof a);
    a.htype = htons(1);
    a.ptype = htons(ETH_P_IP);
    a.hlen = 6;
    a.plen = 4;
    a.oper = htons(oper);
    memcpy(a.sha, eth->dev_addr, 6);
    wr_be32(a.spa, OUR_IP);
    memcpy(a.tha, tha, 6);
    wr_be32(a.tpa, tpa);
    uint8_t dst[6];
    if (oper == ARP_REQUEST)
        memset(dst, 0xFF, 6);
    else
        memcpy(dst, tha, 6);
    eth_send(dst, ETH_P_ARP, &a, sizeof a);
}

/* Frames waiting for an ARP reply. Finished and ready to go: only the
   destination MAC in the first six bytes is still blank. Four is plenty —
   each protocol already retries (TCP's retransmit timer, dns_resolve's
   second try), and every retry re-sends the request. */
#define ARP_QUEUE   4
#define ARP_WAIT_MS 1000

static struct {
    struct sk_buff *skb;
    uint32_t ip;                        /* next hop we are waiting for */
    uint32_t expires;
} pending[ARP_QUEUE];

static int xmit_to(struct sk_buff *skb, const uint8_t mac[6]);

static void arp_expire(uint32_t now)
{
    for (int i = 0; i < ARP_QUEUE; i++)
        if (pending[i].skb && past(pending[i].expires, now)) {
            kfree_skb(pending[i].skb);
            pending[i].skb = NULL;
        }
}

/* Hold one frame and ask who owns `ip`. Never blocks. */
static int arp_queue(struct sk_buff *skb, uint32_t ip)
{
    uint32_t now = timer_ms();
    arp_expire(now);
    int slot = -1;
    for (int i = 0; i < ARP_QUEUE && slot < 0; i++)
        if (!pending[i].skb)
            slot = i;
    if (slot < 0) {                     /* full: the oldest frame gives way */
        slot = 0;
        for (int i = 1; i < ARP_QUEUE; i++)
            if ((int32_t)(pending[i].expires - pending[slot].expires) < 0)
                slot = i;
        kfree_skb(pending[slot].skb);
    }
    pending[slot].skb = skb;
    pending[slot].ip = ip;
    pending[slot].expires = (now + ARP_WAIT_MS) | 1;
    uint8_t zero[6] = { 0 };
    arp_send(ARP_REQUEST, zero, ip);
    return 0;
}

/* Did we ask about this address? A reply we did not ask for is ignored. */
static int arp_awaited(uint32_t ip)
{
    for (int i = 0; i < ARP_QUEUE; i++)
        if (pending[i].skb && pending[i].ip == ip)
            return 1;
    return 0;
}

static void arp_flush(uint32_t ip, const uint8_t mac[6])
{
    for (int i = 0; i < ARP_QUEUE; i++) {
        if (!pending[i].skb || pending[i].ip != ip)
            continue;
        struct sk_buff *skb = pending[i].skb;
        pending[i].skb = NULL;          /* clear first: xmit_to frees it */
        xmit_to(skb, mac);
    }
}

static int on_arp(struct sk_buff *skb)
{
    if (skb->len < sizeof(struct arp_pkt) || skb->dev != eth) {    /* (another device's) */
        kfree_skb(skb);
        return 0;
    }
    struct arp_pkt a;
    memcpy(&a, skb->data, sizeof a);
    kfree_skb(skb);
    if (ntohs(a.htype) != 1 || ntohs(a.ptype) != ETH_P_IP || a.hlen != 6 || a.plen != 4)
        return 0;
    uint32_t spa = rd_be32(a.spa);
    uint32_t tpa = rd_be32(a.tpa);
    uint16_t oper = ntohs(a.oper);
    if (!ip_ok(spa) || !mac_ok(a.sha))
        return 0;

    /* Answer a request for our address straight from the MAC on the frame.
       The asker is not recorded: a request claiming to be the gateway is
       the oldest trick there is. */
    if (oper == ARP_REQUEST && tpa == OUR_IP && ip_rate_ok(spa))
        arp_send(ARP_REPLY, a.sha, spa);

    /* Learn only from a reply to a question of ours. */
    if (oper == ARP_REPLY && tpa == OUR_IP && arp_awaited(spa)) {
        remember(spa, a.sha);
        arp_flush(spa, a.sha);
    }
    return 0;
}

/*
 * One IP ID counter per destination instead of one for the whole machine.
 * A single global counter answered a question nobody should be able to ask:
 * send us two packets, subtract the two ids, and you know exactly how many
 * packets the machine sent to anyone else in between. That is a traffic
 * meter on the whole host, and it is what nmap's idle scan is built on.
 * Per destination, the only count a peer can take is of its own traffic,
 * which it knows already.
 *
 * 64 buckets, chosen by a hash with a secret picked at boot, so an attacker
 * cannot work out which bucket a victim's address lands in and watch it from
 * the side. Two destinations that share a bucket share a counter, which costs
 * nothing but a few skipped ids. The hash is one multiply and a shift because
 * this runs for every packet sent: asking the CSPRNG here would mean a
 * ChaCha20 block per packet, and the ids only need to be unguessable from
 * outside, not independently random.
 */
#define IDENTS 64
static uint32_t idents[IDENTS];
static uint32_t ident_key;

static uint16_t next_ip_id(uint32_t dst)
{
    /* The shift and the table size have to agree, and nothing but this would
       notice if one of them changed: a wrong shift indexes off the end. */
    _Static_assert(IDENTS == 1u << 6, "the shift below must match IDENTS");
    uint32_t h = ((dst ^ ident_key) * 0x9E3779B9u) >> 26;   /* the top 6 bits */
    return (uint16_t)idents[h]++;
}

/* The Ethernet and IP headers are written straight into the skb in front
   of the payload: one copy of the payload, no 1500-byte buffer on the stack.
   The destination MAC is left blank for xmit_to, so a frame can be built
   before we know it. The caller's payload is copied here and nowhere else,
   which is what lets ip_send return without blocking. */
static struct sk_buff *ip_frame(uint32_t src, uint32_t dst, uint8_t proto,
                                const void *payload, uint32_t len)
{
    if (!eth || len > ETH_DATA_LEN - sizeof(struct iphdr))
        return NULL;
    uint32_t total = sizeof(struct iphdr) + len;
    struct sk_buff *skb = alloc_skb(2 + ETH_HLEN + total, 0);
    if (!skb)
        return NULL;
    skb_reserve(skb, 2);
    unsigned char *p = skb_put(skb, ETH_HLEN + total);
    memset(p, 0, 6);
    memcpy(p + 6, eth->dev_addr, 6);
    p[12] = (uint8_t)(ETH_P_IP >> 8);
    p[13] = (uint8_t)ETH_P_IP;
    struct iphdr *ip = (struct iphdr *)(p + ETH_HLEN);
    memset(ip, 0, sizeof *ip);
    ip->ver_ihl = 0x45;
    ip->tot_len = htons((uint16_t)total);
    ip->id = htons(next_ip_id(dst));
    ip->ttl = 64;
    ip->protocol = proto;
    ip->saddr = htonl(src);
    ip->daddr = htonl(dst);
    ip->check = htons(checksum(ip, sizeof *ip));
    memcpy(p + ETH_HLEN + sizeof *ip, payload, len);
    skb->dev = eth;
    return skb;
}

static int xmit_to(struct sk_buff *skb, const uint8_t mac[6])
{
    memcpy(skb->data, mac, 6);
    int rc = dev_queue_xmit(skb);
    if (rc == -EAGAIN)
        kfree_skb(skb);             /* the driver did not take it */
    return rc;
}

/* For a station whose MAC we already have on the frame it sent us. */
static int ip_send_mac(uint32_t dst, const uint8_t mac[6], uint8_t proto,
                       const void *payload, uint32_t len)
{
    struct sk_buff *skb = ip_frame(OUR_IP, dst, proto, payload, len);
    if (!skb)
        return -EMSGSIZE;
    return xmit_to(skb, mac);
}

/* Returns 0 once the frame is the network's problem — sent, or waiting for
   an ARP reply that will send it. Never blocks, so it is safe to call from
   the receive path and from a caller whose payload buffer is reused. */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len)
{
    /* No address yet means no route: a packet with a source of 0.0.0.0 would
       be answered to nowhere, and asking the gateway before we know it is
       asking 0.0.0.0. Only ip_send_bootp may send before a lease. */
    if (!OUR_IP)
        return -ENETDOWN;
    uint32_t next = ((dst ^ OUR_IP) & OUR_MASK) ? OUR_GW : dst;
    if (!ip_ok(next))
        return -EINVAL;
    struct sk_buff *skb = ip_frame(OUR_IP, dst, proto, payload, len);
    if (!skb)
        return -EMSGSIZE;
    uint8_t mac[6];
    if (lookup(next, mac) == 0)
        return xmit_to(skb, mac);
    return arp_queue(skb, next);
}

/*
 * The one send that is allowed before we have an address: see include/ipv4.h.
 * A source of 0.0.0.0 and a destination of 255.255.255.255, to the broadcast
 * MAC, so no ARP is needed -- which matters, because ARP would have to send
 * from an address we do not have.
 */
int ip_send_bootp(const void *udp, uint32_t len)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    struct sk_buff *skb = ip_frame(0, 0xFFFFFFFFu, IPPROTO_UDP, udp, len);
    if (!skb)
        return -EMSGSIZE;
    return xmit_to(skb, bcast);
}

static int on_ip(struct sk_buff *skb)
{
    if (skb->len < sizeof(struct iphdr) || skb->dev != eth) {      /* (another device's) */
        kfree_skb(skb);
        return 0;
    }
    struct iphdr ip;
    memcpy(&ip, skb->data, sizeof ip);
    uint32_t ihl = (ip.ver_ihl & 0x0F) * 4;
    uint32_t total = ntohs(ip.tot_len);
    if ((ip.ver_ihl >> 4) != 4 || ihl != 20 || total < ihl || total > skb->len) {
        kfree_skb(skb);
        return 0;
    }
    if (ip.frag & htons(0x3FFF)) {      /* a fragment: drop */
        kfree_skb(skb);
        return 0;
    }
    uint16_t saved = ip.check;
    ip.check = 0;
    if (checksum(&ip, sizeof ip) != ntohs(saved)) {
        kfree_skb(skb);
        return 0;
    }
    if (!ip_source_ok(ntohl(ip.saddr))) {
        kfree_skb(skb);
        return 0;
    }
    /*
     * Addressed to us, and only to us. Answering a broadcast ping makes this
     * machine a smurf amplifier, and a bound UDP port cannot tell a broadcast
     * datagram from a unicast one, so broadcast is dropped.
     *
     * DHCP is the exception, because a server has no way to reach a machine
     * that has no address yet: its reply comes to 255.255.255.255. The
     * exception is as narrow as it can be made -- only while a transaction is
     * open (dhcp_open(), a few seconds at boot and never again), only UDP,
     * and the datagram still has to reach a bound port, which is 68. A
     * broadcast ping during those few seconds is dropped below like any
     * other, because only UDP gets this far.
     */
    uint32_t dst = ntohl(ip.daddr);
    if (dst != OUR_IP &&
        !(dst == 0xFFFFFFFFu && ip.protocol == IPPROTO_UDP && dhcp_open())) {
        kfree_skb(skb);
        return 0;
    }
    if (dst != OUR_IP) {                /* a BOOTP broadcast: UDP and nothing else */
        udp_input(ntohl(ip.saddr), dst, skb->data + ihl, total - ihl);
        kfree_skb(skb);
        return 0;
    }
    if (ip.protocol == IPPROTO_ICMP && total >= ihl + sizeof(struct icmp_hdr)) {
        struct icmp_hdr ic;
        memcpy(&ic, skb->data + ihl, sizeof ic);
        uint32_t body = total - ihl;
        if (checksum(skb->data + ihl, body) != 0) {
            kfree_skb(skb);
            return 0;
        }
        raw_icmp_input(skb->data, total);   /* ping(8) reads these */
        /* Only the host we pinged can answer our ping. */
        if (ic.type == ICMP_ECHO_REPLY && ntohs(ic.seq) == ping_seq &&
            ntohl(ip.saddr) == ping_dst)
            ping_got = 1;
        /* Static: net_rx_action has one caller at a time. A bigger echo is
           still passed to raw sockets above, we just do not reflect it. */
        static uint8_t reply[1400];
        /* An echo reply goes to whoever the source address says, which an
           attacker chooses, so this is a reflector and needs a budget. */
        if (ic.type == ICMP_ECHO && body <= sizeof reply &&
            ip_rate_ok(ntohl(ip.saddr))) {
            memcpy(reply, skb->data + ihl, body);
            struct icmp_hdr *o = (struct icmp_hdr *)reply;
            o->type = ICMP_ECHO_REPLY;
            o->check = 0;
            o->check = htons(checksum(reply, body));
            /* Reply to the station that asked. Its MAC is already on the
               frame, so this does not wait for ARP inside the receive path. */
            ip_send_mac(ntohl(ip.saddr), skb->eth_src, IPPROTO_ICMP, reply, body);
        }
    } else if (ip.protocol == IPPROTO_UDP) {
        udp_input(ntohl(ip.saddr), dst, skb->data + ihl, total - ihl);
    } else if (ip.protocol == IPPROTO_TCP) {
        tcp_input(ntohl(ip.saddr), dst, skb->data + ihl, total - ihl);
    }
    kfree_skb(skb);
    return 0;
}

static struct packet_type arp_pt = { .type = ETH_P_ARP, .func = on_arp };
static struct packet_type ip_pt = { .type = ETH_P_IP, .func = on_ip };

uint16_t ip_ephemeral_port(int (*used)(uint16_t))
{
    /*
     * RFC 6056. The local port is half of the four-tuple an off-path attacker
     * has to guess, and a counter from 49152 gave them that half for free
     * after seeing a single connection. Pure random choice (the RFC's
     * algorithm 1), not a random start followed by a scan: with a scan,
     * knowing one port tells you where the next one will be.
     *
     * 16384 ports against a couple of dozen sockets, so a collision is about
     * one in a thousand and 32 tries will not be exhausted in practice. The
     * scan after it is there so the function is correct rather than merely
     * very likely. Both loops are bounded: a syscall must not be able to spin.
     */
    for (int i = 0; i < 32; i++) {
        /* 65536 - 49152 is a power of two, so the mask has no modulo bias. */
        uint16_t p = (uint16_t)(49152 + (random_u32() & 16383));
        if (!used(p))
            return p;
    }
    uint32_t start = random_u32() & 16383;
    for (uint32_t i = 0; i < 16384; i++) {
        uint16_t p = (uint16_t)(49152 + ((start + i) & 16383));
        if (!used(p))
            return p;
    }
    return 0;
}

/*
 * Rate limiting what we send only because something arrived.
 *
 * Every one of those is an amplifier: an attacker spoofing a victim's source
 * address gets us to send the victim a packet for free, and an attacker
 * measuring our replies gets a clock. A token bucket caps both.
 *
 * Three decisions matter more than the numbers.
 *
 * The bucket is keyed on **where the reply is going**, not on where the
 * request appeared to come from — which here are the same address, and that
 * is the point. The attacker picks that address, so keying on it means the
 * party being protected is the victim they are aiming at. An attacker who
 * spoofs a thousand different sources does get a thousand buckets, but each
 * of those replies goes to a different place, so there is no flood to aim.
 *
 * It is **per peer** rather than one counter, because a single global counter
 * is itself an oracle: an attacker who can watch the limit run out learns
 * that somebody else is talking to us too, which is precisely the side
 * channel behind CVE-2016-5696 and SAD DNS.
 *
 * And the budget is **jittered**, because a limit that cuts off at exactly N
 * packets is a clean edge to measure against. With the cutoff moving, the
 * measurement returns noise instead of an answer.
 *
 * Costs: 16 slots of 12 bytes, and on the receive path one hash, one compare
 * and a subtract. Residual weakness, written down rather than hidden: an
 * attacker who can spoof source addresses can push a victim out of the table,
 * and two peers can land in the same slot. Both are better than no limit, and
 * neither is fixable without per-peer state we cannot afford.
 */
#define RL_SLOTS  16
#define RL_RATE   64                /* packets per second we will answer */
#define RL_BURST  64                /* and how many of them back to back */

struct rl_slot {
    uint32_t peer;
    uint32_t tokens;
    uint32_t cap;                   /* jittered, 48..64: the moving cutoff */
    uint32_t last_ms;
};
static struct rl_slot rl[RL_SLOTS];
static uint32_t rl_key;
static uint32_t rl_bits, rl_bits_left;

/* Four unpredictable bits. The jitter is worthless if it can be predicted,
   so it comes from the CSPRNG — but a block per packet would be absurd on the
   receive path, so one 32-bit draw serves eight of these, and a draw happens
   only when a slot changes hands or its bucket tops out. */
static uint32_t rl_jitter(void)
{
    if (!rl_bits_left) {
        rl_bits = random_u32();
        rl_bits_left = 8;
    }
    uint32_t v = rl_bits & 15;
    rl_bits >>= 4;
    rl_bits_left--;
    return v;
}

int ip_rate_ok(uint32_t peer)
{
    _Static_assert(RL_SLOTS == 1u << 4, "the shift below must match RL_SLOTS");
    struct rl_slot *s = &rl[((peer ^ rl_key) * 0x9E3779B9u) >> 28];
    uint32_t now = timer_ms();
    if (s->peer != peer) {              /* a new peer takes the slot over */
        s->peer = peer;
        s->cap = RL_BURST - rl_jitter();
        s->tokens = s->cap;
        s->last_ms = now;
    } else {
        uint32_t dt = now - s->last_ms;     /* unsigned: survives the wrap */
        if (dt > 1000)
            dt = 1000;                      /* idle long enough is just full */
        uint32_t gained = dt * RL_RATE / 1000;
        if (gained) {                       /* under 16 ms: let it accumulate */
            s->last_ms = now;
            s->tokens += gained;
            if (s->tokens >= s->cap) {      /* topped out: move the cutoff */
                s->cap = RL_BURST - rl_jitter();
                s->tokens = s->cap;
            }
        }
    }
    if (!s->tokens)
        return 0;
    s->tokens--;
    return 1;
}

/* Four decimals and three dots. The casts are not decoration: uint32_t is
   `unsigned long` on i686-elf, and %u would read the wrong width. */
static void print_ip(const char *label, uint32_t ip)
{
    klog("%s%u.%u.%u.%u", label, (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 0xFF,
            (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
}

/* eth0's address is settled (a lease, or the fixed fallback): keep it with
   the device, use it if eth0 is still the one IPv4 runs on (Wi-Fi may have
   come up meanwhile), and say so. */
static void eth0_settled(struct net_device *dev, const struct dhcp_lease *l)
{
    dev->in4.addr = l->addr;
    dev->in4.mask = l->mask;
    dev->in4.gw = l->gw;
    dev->in4.dns = l->dns;
    if (eth == dev) {
        ipv4_addr = l->addr;
        ipv4_mask = l->mask;
        ipv4_gw = l->gw;
        ipv4_dns = l->dns;
    }
    int bits = 0;
    for (uint32_t m = l->mask; m & 0x80000000u; m <<= 1)
        bits++;
    print_ip("ipv4: eth0 is ", l->addr);
    klog("/%d", bits);
    print_ip(" gateway ", l->gw);
    print_ip(" dns ", l->dns);
    klog("\n");
}

/* DHCP for eth0, or QEMU's fixed addresses if nobody answers. */
static void eth0_lease(struct net_device *dev)
{
    struct dhcp_lease l;
    if (dhcp_lease(dev, &l) < 0) {
        print_ip("dhcp: no answer, using ", IPV4_ADDR_STATIC);
        klog("\n");
        l = (struct dhcp_lease){ IPV4_ADDR_STATIC, IPV4_MASK_STATIC, IPV4_GW_STATIC,
                                 IPV4_DNS_STATIC };
    }
    eth0_settled(dev, &l);
}

/* At boot, in a thread of its own: the exchange takes a while (1 s in QEMU,
   1-3 s on a real network, all of DHCP_TRIES on one that never answers),
   and the boot needn't wait for it. The address arrives while runit and X
   start; a program that wants the network meanwhile is where it would be
   on a slow network (the DNS server comes live from /proc/net/dns). */
static void eth0_lease_thread(void *arg)
{
    irq_save();                         /* (like the net thread: no interrupt races it) */
    eth0_lease(arg);
    thread_exit();
}

void ipv4_init(void)
{
    /* Unguessable before the first packet: the IP ID counters start at a
       random value each, and both hash keys are secret. */
    random_bytes(idents, sizeof idents);
    ident_key = random_u32();
    rl_key = random_u32();
    /* The handlers first, always: a machine whose only network is Wi-Fi has
       no eth0 now and needs them later; with eth0, the replies to the DHCP
       exchange arrive through them. */
    dev_add_pack(&arp_pt);
    dev_add_pack(&ip_pt);
    eth = dev_get_by_name("eth0");
    if (!eth)
        return;
#if defined(SELFTEST) || defined(BENCH)
    eth0_lease(eth);                    /* their network tests start at once */
#else
    if (!thread_create("ipconfig", eth0_lease_thread, eth))
        eth0_lease(eth);                /* no thread: wait for it, as it used to */
#endif
}

/* --- which device IPv4 runs on ------------------------------------------------- */

/* Move to dev, keeping the old device's lease with it for later. The
   neighbour table belonged to the old link: forgotten. */
static void activate(struct net_device *dev)
{
    if (eth) {
        eth->in4.addr = ipv4_addr;
        eth->in4.mask = ipv4_mask;
        eth->in4.gw = ipv4_gw;
        eth->in4.dns = ipv4_dns;
    }
    eth = dev;
    ipv4_addr = dev ? dev->in4.addr : 0;
    ipv4_mask = dev && dev->in4.mask ? dev->in4.mask : IPV4_MASK_STATIC;
    ipv4_gw = dev ? dev->in4.gw : 0;
    ipv4_dns = dev ? dev->in4.dns : 0;
    memset(table, 0, sizeof table);
}

/* DHCP for a device that just became active, in a thread of its own: the
   exchange waits for answers, up to seconds. */
static void lease_thread(void *arg)
{
    struct net_device *dev = arg;
    irq_save();                         /* (like the net thread: no interrupt races it) */
    struct dhcp_lease l;
    if (dhcp_lease(dev, &l) == 0 && eth == dev) {
        dev->in4.addr = ipv4_addr = l.addr;
        dev->in4.mask = ipv4_mask = l.mask;
        dev->in4.gw = ipv4_gw = l.gw;
        dev->in4.dns = ipv4_dns = l.dns;
        print_ip("ipv4: ", ipv4_addr);
        klog(" on %s", dev->name);
        print_ip(" gateway ", ipv4_gw);
        print_ip(" dns ", ipv4_dns);
        klog("\n");
    }
    thread_exit();
}

void ipv4_link_up(struct net_device *dev)
{
    if (!dev || dev == eth)
        return;
    activate(dev);
    klog("ipv4: now on %s\n", dev->name);
    if (!dev->in4.addr)
        thread_create("ipconfig", lease_thread, dev);
}

void ipv4_link_down(struct net_device *dev)
{
    if (!dev)
        return;
    memset(&dev->in4, 0, sizeof dev->in4);     /* (its lease was for that network) */
    if (dev != eth)
        return;
    struct net_device *next = NULL;
    for (int i = 1; i <= NETDEV_MAX && !next; i++) {
        struct net_device *d = dev_get_by_index(i);
        if (d && d != dev && (d->flags & IFF_UP) && !(d->flags & IFF_LOOPBACK) && d->in4.addr)
            next = d;
    }
    eth = NULL;                         /* (dev's settings are cleared, not saved) */
    activate(next);
    klog("ipv4: %s gone, now on %s\n", dev->name, next ? next->name : "nothing");
}

int net_ping(uint32_t dst)
{
    if (!eth)
        return -ENODEV;
    struct icmp_hdr ic;
    memset(&ic, 0, sizeof ic);
    ic.type = ICMP_ECHO;
    ic.id = htons(1);
    ping_seq++;
    ic.seq = htons(ping_seq);
    uint8_t pkt[sizeof ic + 32];
    memcpy(pkt, &ic, sizeof ic);
    memset(pkt + sizeof ic, 0x5A, 32);
    struct icmp_hdr *h = (struct icmp_hdr *)pkt;
    h->check = htons(checksum(pkt, sizeof pkt));
    ping_got = 0;
    ping_dst = dst;
    /* Three tries, 500 ms apart. The first can be spent learning the MAC,
       and a NIC may hold received frames for a while after it is enabled
       (QEMU's e1000: one second). Same sequence number each time, so a
       late reply to an earlier try still counts. */
    for (int tries = 0; tries < 3; tries++) {
        if (ip_send(dst, IPPROTO_ICMP, pkt, sizeof pkt) < 0)
            return -EIO;
        uint32_t t0 = timer_ms();
        while (timer_ms() - t0 < 500) {
            net_rx_action();
            if (ping_got)
                return 0;
            net_sleep(10);
        }
    }
    return -EIO;
}
