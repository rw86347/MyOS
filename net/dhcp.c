/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * dhcp.c — a DHCP client (RFC 2131), enough to be given an address.
 *
 * Four packets: we broadcast a DISCOVER, a server offers an address, we
 * broadcast a REQUEST naming that server, it acknowledges. Two round trips
 * rather than one because a network may hold several servers and they all
 * offer; the REQUEST says which offer was taken, so the others can put their
 * address back.
 *
 * Why a machine with no address can talk at all: the DISCOVER goes from
 * 0.0.0.0 to 255.255.255.255 inside an Ethernet broadcast, so no ARP and no
 * routing is involved (ip_send_bootp in net/ipv4.c is the only path that will
 * do this). We set the BROADCAST flag in the request, which asks the server
 * to broadcast its replies too — otherwise it would have to unicast to an
 * address we have not got yet, which on a real network means the reply is
 * addressed to a machine the switch has never heard of.
 *
 * Everything in a reply is attacker-chosen. Anyone on the segment can answer
 * a broadcast, and nothing in DHCP authenticates a server, so the fields here
 * are parsed the way every other wire format in this kernel is: lengths are
 * checked against the bytes we actually have before they are used, and an
 * option that runs off the end ends the walk. What a hostile server can do is
 * give us a wrong gateway or nameserver — that is inherent to DHCP, not
 * something this code can check, and it is the reason the parser is the only
 * part a fuzzer can reach without a network (tools/fuzz).
 *
 * It does not renew. A lease has a time on it and we ignore it; see
 * docs/gaps.md. On the networks this boots on (QEMU, UTM, a home router) the
 * lease outlasts the session, and renewal needs a timer that can reconfigure
 * sockets that are already open, which is a bigger change than it looks.
 */
#include <dhcp.h>
#include <errno.h>
#include <ipv4.h>
#include <kprintf.h>
#include <netdevice.h>
#include <random.h>
#include <string.h>
#include <timer.h>
#include <udp.h>

#define BOOTP_SERVER 67
#define BOOTP_CLIENT 68

/* Offsets into the fixed part of a BOOTP packet (RFC 951 / 2131 figure 1). */
#define B_OP      0
#define B_HTYPE   1
#define B_HLEN    2
#define B_XID     4
#define B_FLAGS  10
#define B_YIADDR 16
#define B_CHADDR 28
#define B_COOKIE 236
#define B_OPTS   240
/* A BOOTP packet is 300 bytes at the least: the field layout ends at 236, but
   relays and older servers were written to a 300-byte minimum and some still
   drop anything shorter. Padding to it costs nothing. */
#define B_MIN    300

#define OP_REQUEST 1
#define OP_REPLY   2

#define OPT_PAD       0
#define OPT_NETMASK   1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_REQ_IP   50
#define OPT_MSG_TYPE 53
#define OPT_SERVER   54
#define OPT_PARAMS   55
#define OPT_END     255

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

/* Three tries at each of the two steps, 1 second apart: six seconds of boot
   in the worst case, on a network with no server. RFC 2131 wants randomised
   exponential backoff over a much longer period, which is right for a laptop
   that has to keep trying and wrong for a boot that has to finish. */
/*
 * How long to wait for a server, and how patiently.
 *
 * RFC 2131 4.1 asks for retransmission with exponential backoff, and the
 * reason is nothing to do with congestion here: a real DHCP server is a
 * program on another machine that may be busy, and a switch between us and it
 * may still be learning the port. Three tries a second apart was enough for
 * QEMU's built-in server, which answers in microseconds, and would have given
 * up before a real one on a real network got a word in -- the fallback
 * address would then look like a working lease, because under QEMU they are
 * the same four numbers.
 *
 * 1s, 2s, 4s, 8s: fifteen seconds before giving up. It costs nothing when a
 * server is there (the first reply ends the wait immediately), and the only
 * machine that pays the fifteen seconds is one with no server at all, which
 * has a broken network and a boot-time problem either way.
 */
#define DHCP_TRIES    4
#define DHCP_WAIT_MS  1000

static uint32_t xid;                /* this transaction, from the CSPRNG */
static int open_now;                /* dhcp_open(): the broadcast exception */
static int want;                    /* the reply type we are waiting for */
static int got;                     /* the type that arrived */
static uint32_t offer_addr, offer_mask, offer_gw, offer_dns, offer_server;

int dhcp_open(void)
{
    return open_now;
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/*
 * Walk the options of a reply. `len` is how many bytes we really have, so
 * every read below is inside the packet: an option header needs two bytes, and
 * its body needs `l` more. Anything that does not fit ends the walk rather
 * than being clamped, because a truncated option means the packet is not what
 * it says it is and the rest of it cannot be trusted either.
 */
static void read_options(const uint8_t *p, uint32_t len)
{
    for (uint32_t i = B_OPTS; i < len; ) {
        uint8_t code = p[i];
        if (code == OPT_END)
            return;
        if (code == OPT_PAD) {
            i++;                    /* one byte, no length, no body */
            continue;
        }
        if (i + 2 > len)
            return;                 /* no room for the length byte */
        uint8_t l = p[i + 1];
        if (i + 2 + l > len)
            return;                 /* body runs off the end */
        const uint8_t *v = p + i + 2;
        switch (code) {
        case OPT_MSG_TYPE:
            if (l == 1)
                got = v[0];
            break;
        case OPT_NETMASK:
            if (l == 4)
                offer_mask = rd_be32(v);
            break;
        case OPT_ROUTER:
            if (l >= 4)
                offer_gw = rd_be32(v);      /* the first of the list */
            break;
        case OPT_DNS:
            if (l >= 4)
                offer_dns = rd_be32(v);
            break;
        case OPT_SERVER:
            if (l == 4)
                offer_server = rd_be32(v);
            break;
        default:
            break;                  /* a server may send anything; ignore it */
        }
        i += 2u + l;
    }
}

void dhcp_input(uint32_t src, uint16_t sport, const uint8_t *pkt, uint32_t len)
{
    (void)src;
    if (!open_now || sport != BOOTP_SERVER)
        return;
    /* B_OPTS, not B_MIN: the minimum applies to what we send. A reply has to
       reach the cookie for anything after it to mean anything. */
    if (len < B_OPTS || pkt[B_OP] != OP_REPLY || rd_be32(pkt + B_XID) != xid)
        return;
    /* Without the cookie the four bytes at 236 are a BOOTP vendor field and
       the bytes after it are not options at all. */
    if (rd_be32(pkt + B_COOKIE) != 0x63825363u)
        return;
    got = 0;
    offer_mask = offer_gw = offer_dns = offer_server = 0;
    read_options(pkt, len);
    if (got != want)
        return;
    uint32_t yi = rd_be32(pkt + B_YIADDR);
    /* An address of zero, a broadcast or a multicast would make every send
       from here on invalid, and 127/8 would send our traffic to ourselves.
       A server offering one is broken or hostile; either way, keep waiting. */
    if (!yi || yi == 0xFFFFFFFFu || (yi >> 28) == 0xE || (yi >> 24) == 127) {
        got = 0;
        return;
    }
    offer_addr = yi;
}

/* One option: code, length, body. `at` is where to write; returns the next. */
static uint32_t put_opt(uint8_t *p, uint32_t at, uint8_t code,
                        const void *body, uint8_t l)
{
    p[at] = code;
    p[at + 1] = l;
    if (l)
        memcpy(p + at + 2, body, l);
    return at + 2u + l;
}

/* Build and broadcast a DISCOVER or a REQUEST. */
static int send_msg(const uint8_t mac[6], int type)
{
    /* 8 bytes of UDP header and then the BOOTP packet, built together so
       there is one buffer and one copy. On the stack: this runs at boot from
       one thread, and 316 bytes is well inside a kernel stack. */
    uint8_t buf[8 + B_MIN];
    memset(buf, 0, sizeof buf);
    uint8_t *p = buf + 8;

    p[B_OP] = OP_REQUEST;
    p[B_HTYPE] = 1;                 /* Ethernet */
    p[B_HLEN] = 6;
    wr_be32(p + B_XID, xid);
    /* Ask for the replies to be broadcast; see the top of the file. */
    p[B_FLAGS] = 0x80;
    memcpy(p + B_CHADDR, mac, 6);
    wr_be32(p + B_COOKIE, 0x63825363u);

    uint8_t t = (uint8_t)type;
    uint32_t at = put_opt(p, B_OPTS, OPT_MSG_TYPE, &t, 1);
    if (type == DHCP_REQUEST) {
        /* Which offer we took, and from whom. A server that sees its own
           address here keeps the lease; the others release theirs. */
        uint8_t a[4];
        wr_be32(a, offer_addr);
        at = put_opt(p, at, OPT_REQ_IP, a, 4);
        wr_be32(a, offer_server);
        at = put_opt(p, at, OPT_SERVER, a, 4);
    }
    static const uint8_t params[] = { OPT_NETMASK, OPT_ROUTER, OPT_DNS };
    at = put_opt(p, at, OPT_PARAMS, params, sizeof params);
    p[at++] = OPT_END;
    /* Pad to the 300-byte minimum rather than sending what we filled. */
    uint32_t blen = at > B_MIN ? at : B_MIN;

    uint16_t ulen = (uint16_t)(8 + blen);
    buf[0] = (uint8_t)(BOOTP_CLIENT >> 8);
    buf[1] = (uint8_t)BOOTP_CLIENT;
    buf[2] = (uint8_t)(BOOTP_SERVER >> 8);
    buf[3] = (uint8_t)BOOTP_SERVER;
    buf[4] = (uint8_t)(ulen >> 8);
    buf[5] = (uint8_t)ulen;
    uint16_t sum = ip_transport_csum(0, 0xFFFFFFFFu, IPPROTO_UDP, buf, ulen);
    if (sum == 0)
        sum = 0xFFFF;
    buf[6] = (uint8_t)(sum >> 8);
    buf[7] = (uint8_t)sum;
    return ip_send_bootp(buf, ulen);
}

/* Broadcast `type` up to DHCP_TRIES times and wait for `reply`, waiting twice
   as long after each. 0 if it came.
   Same xid every try, so a slow answer to an earlier one still counts -- which
   is the point of the backoff: the later waits are long enough to catch a
   reply to the first request rather than only to the last. */
static int exchange(const uint8_t mac[6], int type, int reply)
{
    want = reply;
    got = 0;
    uint32_t wait = DHCP_WAIT_MS;
    for (int tries = 0; tries < DHCP_TRIES; tries++, wait *= 2) {
        if (send_msg(mac, type) < 0)
            return -EIO;
        uint32_t t0 = timer_ms();
        while (timer_ms() - t0 < wait) {
            /* Nothing else is running the receive path yet: the net thread is
               started after ipv4_init returns. Same shape as net_ping. */
            net_rx_action();
            if (got == reply)
                return 0;
            net_sleep(10);
        }
    }
    return -ETIMEDOUT;
}

/* The casts matter: uint32_t is `unsigned long` on i686-elf and %u would read
   the wrong width. */
static void print_ip(const char *label, uint32_t ip)
{
    klog("%s%u.%u.%u.%u", label, (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 0xFF,
            (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
}

static void use_static(void)
{
    ipv4_addr = IPV4_ADDR_STATIC;
    ipv4_mask = IPV4_MASK_STATIC;
    ipv4_gw = IPV4_GW_STATIC;
    ipv4_dns = IPV4_DNS_STATIC;
}

static void recv_cb(void *ctx, uint32_t src, uint16_t sport,
                    const uint8_t *data, uint32_t len)
{
    (void)ctx;
    dhcp_input(src, sport, data, len);
}

/* Ask dev's network for a lease (dev must be the device IPv4 runs on: the
   exchange goes out through it). 0 and *out, or -errno. */
int dhcp_lease(struct net_device *dev, struct dhcp_lease *out)
{
    int rc = udp_bind(BOOTP_CLIENT, recv_cb, &open_now);
    if (rc < 0)
        return rc;
    /* A guessable transaction id lets anyone on the segment answer a request
       they never saw, which is the whole of DHCP's (weak) defence. */
    xid = random_u32();
    open_now = 1;
    rc = exchange(dev->dev_addr, DHCP_DISCOVER, DHCP_OFFER);
    if (rc == 0) {
        /* The REQUEST has to name the address from the offer, so keep it
           across the second exchange: a reply to the request overwrites
           offer_addr with the same value, and a NAK leaves it alone. */
        uint32_t asked = offer_addr;
        rc = exchange(dev->dev_addr, DHCP_REQUEST, DHCP_ACK);
        if (rc == 0 && offer_addr != asked)
            rc = -EINVAL;           /* acknowledged something we did not ask for */
    }
    open_now = 0;
    udp_unbind(BOOTP_CLIENT);
    if (rc < 0)
        return rc;
    /* Said out loud: under QEMU the static fallback below produces exactly
       the same four addresses, so without this line a boot log cannot tell
       a working DHCP client from one that never got a reply. */
    print_ip("dhcp: lease from ", offer_server);
    klog("\n");
    out->addr = offer_addr;
    /* A server need not send a netmask or a gateway, and one that sends a
       netmask of zero would put every address off-subnet. Fall back to the
       class-C guess, which is right on every network this runs on. */
    out->mask = offer_mask ? offer_mask : IPV4_MASK_STATIC;
    out->gw = offer_gw;
    /* No nameserver offered: try the gateway, which is usually also a
       resolver. Better than leaving DNS pointed at nothing. */
    out->dns = offer_dns ? offer_dns : offer_gw;
    return 0;
}

int dhcp_configure(void)
{
    struct net_device *eth = dev_get_by_name("eth0");
    struct dhcp_lease l;
    int rc = eth ? dhcp_lease(eth, &l) : -ENODEV;
    if (rc < 0) {
        if (eth) {
            print_ip("dhcp: no answer, using ", IPV4_ADDR_STATIC);
            klog("\n");
        }
        use_static();
        return rc;
    }
    ipv4_addr = l.addr;
    ipv4_mask = l.mask;
    ipv4_gw = l.gw;
    ipv4_dns = l.dns;
    return 0;
}
