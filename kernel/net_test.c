/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * net_test.c — the packet path and the PCI scan (make test, step 10)
 *
 * Loopback: build an Ethernet frame, send it on "lo", and check the
 * protocol handler gets the payload and then frees the skb.
 *
 * NAPI: schedule a poll and check net_rx_action runs it once.
 *
 * PCI: QEMU's e1000 is vendor 0x8086, device 0x100e (82540EM).
 *
 * The card: ICMP echo to 10.0.2.2, which is QEMU's user-network gateway.
 * A reply means a frame left the e1000 and one came back. Loopback does
 * not count as that.
 */
#include <dns.h>
#include <errno.h>
#include <e1000.h>
#include <interrupts.h>
#include <ipv4.h>
#include <tcp.h>
#include <kprintf.h>
#include <netdevice.h>
#include <pci.h>
#include <selftest.h>
#include <string.h>

static int saw;
static unsigned char first;

static int recv(struct sk_buff *skb)
{
    saw = (int)skb->len;
    first = skb->len ? skb->data[0] : 0;
    consume_skb(skb);
    return 0;
}

static int polls;
static void tcp_listen_selftest(void);

static int test_poll(struct napi_struct *napi, int budget)
{
    (void)budget;
    polls++;
    napi_complete_done(napi, 1);
    return 1;
}

void net_selftest(void)
{
    uint32_t before = skb_live();

    struct packet_type pt = { .type = 0x9000, .func = recv };
    check(dev_add_pack(&pt) == 0, "net: register a protocol");

    struct net_device *lo = dev_get_by_name("lo");
    check(lo && (lo->flags & IFF_UP), "net: loopback is up");
    if (!lo)
        return;

    struct sk_buff *skb = alloc_skb(64, 0);
    check(skb != NULL, "net: alloc a packet");
    if (!skb)
        return;
    skb_reserve(skb, 2);
    unsigned char *p = skb_put(skb, ETH_HLEN + 4);
    check(p != NULL, "net: room for an Ethernet frame");
    if (!p) {
        kfree_skb(skb);
        return;
    }
    memset(p, 0, ETH_HLEN);
    p[12] = 0x90;                       /* type 0x9000, big-endian on the wire */
    p[13] = 0x00;
    p[14] = 0x11;
    p[15] = 0x22;
    p[16] = 0x33;
    p[17] = 0x44;
    skb->dev = lo;

    check(dev_queue_xmit(skb) == 0, "net: send on loopback");
    net_rx_action();
    check(saw == 4 && first == 0x11, "net: loopback delivered the payload");
    if (saw == 4 && first == 0x11)
        kprintf("net: loopback delivered\n");
    check(skb_live() == before, "net: the packet was freed");

    /* Transmit more than a full ring (TX_N = 16) on a quiet link. Interrupts
       are off for the loop, so no RX interrupt fires and the net thread
       cannot run: the only thing that can reclaim a transmit slot is the
       transmit path itself. The card completes each frame as QEMU processes
       the TDT write, so with reaping in e1000_xmit every send finds a free
       slot; without it the ring fills after 15 and the rest return -EAGAIN.
       A bogus locally-administered destination that answers nothing keeps the
       link quiet from our side. */
    struct net_device *eth = dev_get_by_name("eth0");
    if (eth && (eth->flags & IFF_UP)) {
        const int tries = 2 * 32;       /* comfortably past the 16-slot ring */
        int sent = 0;
        uint32_t f = irq_save();
        for (int i = 0; i < tries; i++) {
            struct sk_buff *s = alloc_skb(ETH_ZLEN, 0);
            if (!s)
                break;
            unsigned char *q = skb_put(s, ETH_ZLEN);
            if (!q) {
                kfree_skb(s);
                break;
            }
            memset(q, 0, ETH_ZLEN);
            memset(q, 0x02, ETH_ALEN);              /* bogus unicast dest */
            memcpy(q + 6, eth->dev_addr, ETH_ALEN);
            q[12] = 0x90;                           /* type 0x9000: nobody replies */
            s->dev = eth;
            if (dev_queue_xmit(s) == 0)
                sent++;
            else {
                kfree_skb(s);           /* -EAGAIN: dev_queue_xmit left it to us */
                break;
            }
        }
        irq_restore(f);
        check(sent == tries, "net: e1000 transmit keeps flowing on a quiet link");
        if (sent != tries)
            kprintf("net: e1000 wedged after %d transmits (TX reaping)\n", sent);
    }
    check(skb_live() == before, "net: the quiet-link transmits were all freed");

    static struct napi_struct napi;
    static struct net_device dummy;
    polls = 0;
    netif_napi_add_weight(&dummy, &napi, test_poll, 64);
    napi_enable(&napi);
    napi_schedule(&napi);
    net_rx_action();
    net_rx_action();                    /* already finished: must not poll again */
    check(polls == 1, "net: NAPI poll runs once per schedule");

    struct pci_dev *e1000 = pci_find(0x8086, 0x100e);
    check(e1000 != NULL, "pci: e1000 8086:100e is on the bus");
    if (e1000)
        kprintf("pci: e1000 8086:100e\n");

    /* 10.0.2.2 is the gateway QEMU's user net answers for. */
    int ping = net_ping(0x0A000202u);
    if (ping != 0)
        e1000_debug();
    check(ping == 0, "net: ping 10.0.2.2");
    if (ping == 0)
        kprintf("net: ping 10.0.2.2 ok\n");

    uint32_t ip = 0;
    int dr = dns_resolve("example.com", &ip);
    if (dr != 0)
        kprintf("net: dns failed %d\n", dr);
    check(dr == 0, "net: dns example.com");
    if (dr == 0)
        kprintf("net: dns example.com is %lu.%lu.%lu.%lu\n",
                ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);

    if (dr == 0) {
        static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
        struct tcp_conn *c = tcp_open();
        int tc = c ? tcp_connect(c, ip, 80, 0, 0) : -ENOMEM;
        if (tc != 0)
            kprintf("net: tcp connect failed %d\n", tc);
        check(tc == 0, "net: tcp connect example.com:80");
        if (tc == 0) {
            int ts = tcp_write(c, req, (uint32_t)strlen(req), 0, 0, 0);
            char buf[64];
            /* 5s, not 2s. Fetching this needs a handshake and a round trip
               to a real server, so on a link with a 350ms round trip 2s left
               no room for a single retransmit and the check failed at random.
               The wait ends when the data arrives, so a quick network pays
               nothing for the larger number. */
            int n = ts > 0 ? tcp_read(c, buf, sizeof buf, 0, 0, 0, 5000) : ts;
            tcp_release(c);
            c = 0;
            int http = n >= 5 && memcmp(buf, "HTTP/", 5) == 0;
            if (!http)
                kprintf("net: tcp recv %d\n", n);
            check(http, "net: tcp HTTP response");
            if (http)
                kprintf("net: tcp example.com HTTP ok\n");
        } else if (c) {
            tcp_release(c);
        }
    }
    tcp_listen_selftest();
    check(skb_live() == before, "net: ping did not leak a packet");
}

/*
 * Passive open, end to end. make test forwards the host's port HAIRPIN_PORT
 * to our port 7 (QEMU's hostfwd), and 10.0.2.2 is the host as QEMU's user
 * network shows it. So a connection from here to 10.0.2.2:HAIRPIN_PORT goes
 * out through the e1000, into the host, and back in through the forward to
 * our own listener: both ends of one TCP connection, each through the card.
 * Without the forward (tools/test-disks.sh) the connect is refused and the
 * test says it was skipped. The port must match HAIRPIN_PORT in the Makefile.
 */
#define HAIRPIN_PORT 47007

/* A segment from a stranger, built by hand: a forged handshake ACK. */
static void forged_ack(uint16_t dport)
{
    uint8_t s[20];
    /* 192.0.2.1 (TEST-NET-1): nobody, and off our subnet, so our reset goes
       straight to the gateway rather than waiting in the ARP queue for an
       answer that never comes (and counting as a leaked packet below). */
    uint32_t src = 0xC0000201u;
    memset(s, 0, sizeof s);
    s[0] = 0x04, s[1] = 0xD2;       /* port 1234 */
    s[2] = (uint8_t)(dport >> 8), s[3] = (uint8_t)dport;
    s[4] = 0x11, s[7] = 0x01;       /* seq: anything */
    s[8] = 0x22, s[11] = 0x02;      /* ack: a guess at our cookie */
    s[12] = 5 << 4;
    s[13] = 0x10;                   /* ACK */
    s[14] = 0x40;                   /* window */
    uint16_t sum = ip_transport_csum(src, ipv4_addr, IPPROTO_TCP, s, sizeof s);
    s[16] = (uint8_t)(sum >> 8), s[17] = (uint8_t)sum;
    tcp_input(src, ipv4_addr, s, sizeof s);
}

static void tcp_listen_selftest(void)
{
    struct tcp_conn *l = tcp_open();
    int lr = l ? tcp_bind(l, 7) : -ENOMEM;
    if (lr == 0)
        lr = tcp_listen(l, 1);
    check(lr == 0, "net: tcp listen on port 7");
    if (lr != 0) {
        if (l)
            tcp_release(l);
        return;
    }
    struct tcp_conn *a = 0;
    forged_ack(7);
    check(tcp_accept(l, &a, 1, 0) == -EAGAIN, "net: tcp a forged ACK makes no connection");

    struct tcp_conn *cl = tcp_open();
    int cr = cl ? tcp_connect(cl, 0x0A000202u, HAIRPIN_PORT, 0, 0) : -ENOMEM;
    if (cr != 0) {
        kprintf("net: tcp listen test skipped: no port forward (%d)\n", cr);
    } else {
        char b[16];
        int ar = tcp_accept(l, &a, 0, 5000);
        check(ar == 0, "net: tcp accept");
        int ok = ar == 0 && tcp_write(cl, "ping\n", 5, 0, 0, 0) == 5 &&
                 tcp_read(a, b, sizeof b, 0, 0, 0, 5000) == 5 && !memcmp(b, "ping\n", 5) &&
                 tcp_write(a, "pong\n", 5, 0, 0, 0) == 5 &&
                 tcp_read(cl, b, sizeof b, 0, 0, 0, 5000) == 5 && !memcmp(b, "pong\n", 5);
        check(ok, "net: tcp data both ways on an accepted connection");
        if (ok) {
            tcp_shutdown_write(a);      /* the server hangs up first */
            ok = tcp_read(cl, b, sizeof b, 0, 0, 0, 5000) == 0;
            check(ok, "net: tcp the client sees the server's FIN");
        }
        if (ok)
            kprintf("net: tcp accept ok: ping, pong, and the server's FIN\n");
        if (a)
            tcp_release(a);
    }
    if (cl)
        tcp_release(cl);
    tcp_release(l);
}
