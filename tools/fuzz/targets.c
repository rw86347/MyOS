/*
 * targets.c — the entry points the fuzzer drives, one per attack surface.
 *
 * The hard part of fuzzing a network stack is not generating bytes, it is
 * getting past the gates in front of the interesting code. A random 40-byte
 * string fails the IPv4 header checksum essentially always, so a naive
 * fuzzer spends its whole life watching on_ip drop packets at line four and
 * never reaches a parser. Each target below therefore repairs the fields
 * that are pure bookkeeping — version, header length, our address, the
 * checksums, the ports of a live connection — and leaves everything the
 * attacker actually controls alone.
 *
 * That is a deliberate trade. Repairing a field means no longer testing the
 * check on it, so each repair is a field the `wire` target still covers raw.
 *
 * Targets:
 *   wire  bytes to on_ip with nothing repaired: the drop paths and the
 *         length checks, where Ripple20 lived.
 *   ip    a well-formed IPv4 packet with an attacker-controlled body:
 *         ICMP echo, and dispatch into UDP and TCP.
 *   arp   bytes to on_arp.
 *   dns   a reply to a lookup the stack is really waiting for, with the
 *         transaction id and question filled in so the bytes reach
 *         parse_a/skip_name. This is the AMNESIA:33 surface.
 *   tcp   a sequence of segments against a connection in SYN_SENT or
 *         ESTABLISHED, with the clock advanced between them so retransmit
 *         and timeout code is reachable.
 */

#include <dns.h>
#include <ipv4.h>
#include <netdevice.h>
#include <skbuff.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>
#include <timer.h>
#include <udp.h>

#include "stubs.h"
#include "targets.h"

/* Offsets, rather than structs copied out of net/ipv4.c: these are wire
   formats fixed by RFC 791/792/768/793, and a private copy of a struct
   would be one more thing that can disagree with the kernel's. */
#define IP_VER_IHL  0
#define IP_TOT_LEN  2
#define IP_FRAG     6
#define IP_PROTO    9
#define IP_CHECK   10
#define IP_SADDR   12
#define IP_DADDR   16
#define IP_HLEN    20

#define ETH_SRC     6
#define ETH_TYPE   12

/* Longer than net/tcp.c's ORPHAN_MS and TIME_WAIT_MS, so a released
   connection is certainly due to be reaped. */
#define ORPHAN_SLACK_MS 30000

#define PEER_IP   0x0A000202u       /* 10.0.2.2, the gateway: on our subnet */
#define PEER_PORT 80

static const uint8_t peer_mac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* The kernel's own checksum, reused so the harness cannot disagree with it:
   ip_csum_finish(ip_csum_add(0, ...)) is exactly what net/ipv4.c does. */
static uint16_t csum(const void *d, uint32_t n)
{
    return ip_csum_finish(ip_csum_add(0, d, n));
}

/* --- handing a frame to the stack ----------------------------------------
   Built to look like what eth_type_trans leaves behind: the Ethernet header
   already pulled, skb->protocol set, the sender's MAC saved in eth_src. The
   handler takes ownership and frees it. */
static void feed(uint16_t type, const uint8_t *b, uint32_t n, const uint8_t mac[6])
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
    fn(skb);
}

/* --- ARP ------------------------------------------------------------------ */

static void arp_build(uint8_t out[28], uint16_t oper, const uint8_t sha[6],
                      uint32_t spa, uint32_t tpa)
{
    wr16(out, 1);                   /* Ethernet */
    wr16(out + 2, 0x0800);          /* IPv4 */
    out[4] = 6;
    out[5] = 4;
    wr16(out + 6, oper);
    memcpy(out + 8, sha, 6);
    wr32(out + 14, spa);
    memset(out + 18, 0, 6);
    wr32(out + 24, tpa);
}

/* Answer an ARP request the stack sent. Without this nothing addressed to a
   neighbour ever leaves: ip_send queues the frame and waits for a reply, so
   the DNS query and the TCP SYN would sit in the pending queue and the
   target would never see them. */
static void arp_answer(const uint8_t *arp)
{
    uint32_t want = rd32(arp + 24);         /* tpa: who they are asking about */
    uint8_t reply[28];
    arp_build(reply, 2 /* reply */, peer_mac, want, 0x0A00020Fu /* IPV4_ADDR */);
    feed(ETH_P_ARP, reply, sizeof reply, peer_mac);
}

/* --- the reply pump ------------------------------------------------------
   Called from net_sleep and net_rx_action, i.e. exactly where the stack
   waits for something. Drains everything it sent and answers it. ARP
   replies flush frames that were waiting, so this loops until quiet. */

static void (*on_ip_frame)(const uint8_t *ip, uint32_t len);

static void pump(void)
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
            if (type == ETH_P_ARP && blen >= 28 && rd16(body + 6) == 1)
                arp_answer(body);
            else if (type == ETH_P_IP && blen >= IP_HLEN && on_ip_frame)
                on_ip_frame(body, blen);
        }
        if (!any)
            break;
    }
}

/* --- target: wire -------------------------------------------------------- */

static void t_wire(const uint8_t *d, size_t n)
{
    feed(ETH_P_IP, d, (uint32_t)n, peer_mac);
}

/* --- target: ip ----------------------------------------------------------
   Repairs the header so the packet is addressed to us and passes the
   checksum, then repairs whichever transport checksum the body needs. The
   protocol byte, the body, and every length inside the body stay the
   fuzzer's. */

static void t_ip(const uint8_t *d, size_t n)
{
    if (n < IP_HLEN)
        return;
    if (n > 1600)
        n = 1600;
    uint8_t pkt[1600];
    memcpy(pkt, d, n);

    pkt[IP_VER_IHL] = 0x45;                     /* IPv4, no options */
    wr16(pkt + IP_TOT_LEN, (uint16_t)n);        /* match what we hand over */
    wr16(pkt + IP_FRAG, 0);                     /* not a fragment: all are dropped */
    wr32(pkt + IP_SADDR, PEER_IP);
    wr32(pkt + IP_DADDR, 0x0A00020Fu);          /* IPV4_ADDR: only ours is taken */
    wr16(pkt + IP_CHECK, 0);
    wr16(pkt + IP_CHECK, csum(pkt, IP_HLEN));

    uint8_t  proto = pkt[IP_PROTO];
    uint8_t *seg   = pkt + IP_HLEN;
    uint32_t slen  = (uint32_t)n - IP_HLEN;

    if (proto == IPPROTO_ICMP && slen >= 4) {
        wr16(seg + 2, 0);
        wr16(seg + 2, csum(seg, slen));
    } else if (proto == IPPROTO_TCP && slen >= 20) {
        wr16(seg + 16, 0);
        wr16(seg + 16, ip_transport_csum(PEER_IP, 0x0A00020Fu, IPPROTO_TCP, seg, slen));
    } else if (proto == IPPROTO_UDP && slen >= 8) {
        wr16(seg + 6, 0);           /* UDP may leave it zero; udp_input allows it */
    }

    feed(ETH_P_IP, pkt, (uint32_t)n, peer_mac);
}

/* --- target: arp --------------------------------------------------------- */

/*
 * An address on our subnet that nothing ever answers for, so a lookup for it
 * always fails and always leaves an entry in the pending queue. That matters
 * because the cache only learns from a reply to a request it actually made:
 * with nothing outstanding, arp_awaited() is false, remember() is
 * unreachable, and the fuzzer would only ever test the rejection path.
 */
#define PROBE_IP 0x0A00024Du        /* 10.0.2.77 */

static void t_arp(const uint8_t *d, size_t n)
{
    static const uint8_t pad[8] = { 0 };
    ip_send(PROBE_IP, IPPROTO_UDP, pad, sizeof pad);    /* now awaited */
    struct fuzz_frame f;
    while (fuzz_take(&f))
        ;                           /* drop the request we just made */

    feed(ETH_P_ARP, d, (uint32_t)n, peer_mac);

    while (fuzz_take(&f))
        ;
}

/* --- target: dns ---------------------------------------------------------
   Layout of the input:
     [0]      control: bit 0 keeps the real transaction id
     [1..12]  the DNS header, the fuzzer's (flags, counts)
     [13..]   the answer section, the fuzzer's
   The question section is spliced in by the harness, because on_dns
   byte-compares it and a mismatch drops the reply before parse_a runs.

   An attacker who cannot guess the id and port does not get this far today
   either — but the id is a counter and the stack has no CSPRNG, so assuming
   they are known is the honest model, not a generous one. */

static const uint8_t *dns_in;
static size_t dns_in_n;
static int dns_answered;

static void dns_reply(const uint8_t *ip, uint32_t len)
{
    if (dns_answered || len < IP_HLEN + 8)
        return;
    if (ip[IP_PROTO] != IPPROTO_UDP)
        return;
    const uint8_t *udp = ip + IP_HLEN;
    if (rd16(udp + 2) != 53)                    /* not the query */
        return;
    uint16_t our_port = rd16(udp);
    const uint8_t *q = udp + 8;
    uint32_t qn = len - IP_HLEN - 8;
    if (qn < 12)
        return;
    dns_answered = 1;

    uint8_t ctl = dns_in_n ? dns_in[0] : 0;
    const uint8_t *body = dns_in_n ? dns_in + 1 : dns_in;
    size_t bn = dns_in_n ? dns_in_n - 1 : 0;

    uint8_t hdr[12];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, body, bn < sizeof hdr ? bn : sizeof hdr);
    if (bn > sizeof hdr) {
        body += sizeof hdr;
        bn   -= sizeof hdr;
    } else {
        bn = 0;
    }
    if (ctl & 1) {                              /* the id the stack is waiting for */
        hdr[0] = q[0];
        hdr[1] = q[1];
    }

    /* on_dns drops anything over sizeof dns_pkt (512), so staying under it
       is what keeps the bytes reaching parse_a. */
    uint8_t pkt[8 + 512];
    uint32_t qlen = qn - 12;                    /* the question, verbatim */
    if (qlen > 256)
        return;
    uint32_t o = 8;
    memcpy(pkt + o, hdr, 12);           o += 12;
    memcpy(pkt + o, q + 12, qlen);      o += qlen;
    uint32_t room = (uint32_t)sizeof pkt - o;
    if (bn > room)
        bn = room;
    memcpy(pkt + o, body, bn);          o += (uint32_t)bn;

    wr16(pkt, 53);                              /* from the resolver's port 53 */
    wr16(pkt + 2, our_port);
    wr16(pkt + 4, (uint16_t)o);
    wr16(pkt + 6, 0);                           /* no UDP checksum */
    udp_input(IPV4_DNS, IPV4_ADDR, pkt, o);
}

static void t_dns(const uint8_t *d, size_t n)
{
    dns_in = d;
    dns_in_n = n;
    dns_answered = 0;
    on_ip_frame = dns_reply;
    fuzz_set_inject(pump);

    uint32_t ip = 0;
    int rc = dns_resolve("example.com", &ip);
    if (getenv("FUZZ_VERBOSE"))
        fprintf(stderr, "dns: answered=%d rc=%d ip=%u.%u.%u.%u\n", dns_answered, rc,
                ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

    fuzz_set_inject(NULL);
    on_ip_frame = NULL;
}

/* --- target: tcp --------------------------------------------------------
   Opens a connection, lets it send its SYN, then replays the input as a
   series of segments against it. Input layout, repeated:
     [0]     control: bit 0 repair the checksum, bit 1 use a plausible ack,
             bit 2 advance the clock and run the timers afterwards
     [1]     segment length in bytes (clamped)
     [2..]   the segment
   Ports are always repaired: a segment that misses the connection only ever
   reaches the "no such connection, send a reset" path, which the ip target
   already covers. */

/*
 * What the stack last sent on the connection, so the next injected segment
 * can carry numbers it will accept. Filled in by the pump as frames are
 * drained, because the pump is what drains them — reading the queue
 * afterwards finds it already empty.
 *
 * This models an attacker who can see our traffic. For TCP that is normally
 * a much stronger attacker than an off-path one, but the ISN here is
 * timer_ms() * 64000 + a constant, so an off-path attacker can work these
 * out too. Treating them as known is the threat model, not a shortcut.
 */
static uint32_t our_seq, our_len, their_seq;

static void tcp_record(const uint8_t *ip, uint32_t len)
{
    if (len < IP_HLEN + 20 || ip[IP_PROTO] != IPPROTO_TCP)
        return;
    const uint8_t *t = ip + IP_HLEN;
    uint32_t hl = (uint32_t)(t[12] >> 4) * 4;
    uint32_t total = rd16(ip + IP_TOT_LEN);
    uint8_t flags = t[13];
    our_seq = rd32(t + 4);
    our_len = (hl < 20 || total < IP_HLEN + hl) ? 0 : total - IP_HLEN - hl;
    if (flags & 0x03)                   /* SYN and FIN each take a sequence */
        our_len += 1;
    their_seq = rd32(t + 8);            /* what they are expecting from us */
}

static void t_tcp(const uint8_t *d, size_t n)
{
    struct tcp_conn *c = tcp_open();
    if (!c) {
        /* Not a fuzzer problem: the table is 16 and every run frees its
           connection, so running dry means one leaked. Worth stopping for. */
        fprintf(stderr, "FINDING: tcp_open failed, connections leaked\n");
        abort();
    }

    our_seq = our_len = their_seq = 0;
    on_ip_frame = tcp_record;
    fuzz_set_inject(pump);
    tcp_connect(c, PEER_IP, PEER_PORT, 1 /* nonblock */);
    /* A non-blocking connect returns the moment the SYN is handed to
       ip_send, without ever waiting, so nothing has called pump yet. Do it
       here or the SYN is never drained and its sequence number never read. */
    pump();

    uint16_t lport = 0, rport = 0;
    uint32_t raddr = 0;
    tcp_names(c, &lport, &raddr, &rport);

    size_t i = 0;
    for (int step = 0; step < 16 && i + 2 <= n; step++) {
        uint8_t ctl = d[i++];
        uint32_t slen = d[i++];
        if (slen < 20)
            slen = 20;
        if (i + slen > n)
            slen = (uint32_t)(n - i);
        if (slen < 20)
            break;
        if (slen > 1400)
            slen = 1400;

        uint8_t seg[1400];
        memcpy(seg, d + i, slen);
        i += slen;

        wr16(seg, PEER_PORT);                   /* theirs */
        wr16(seg + 2, lport);                   /* ours */
        if (ctl & 2) {
            wr32(seg + 4, their_seq);
            wr32(seg + 8, our_seq + our_len);
        }
        if (ctl & 1) {
            wr16(seg + 16, 0);
            wr16(seg + 16, ip_transport_csum(PEER_IP, IPV4_ADDR, IPPROTO_TCP, seg, slen));
        }

        tcp_input(PEER_IP, IPV4_ADDR, seg, slen);
        pump();                 /* drain and record whatever it answered */

        if (ctl & 4) {
            fuzz_clock_add(1 + (ctl >> 3) * 250u);
            tcp_timers(timer_ms());
        }
    }

    if (getenv("FUZZ_VERBOSE"))
        fprintf(stderr, "tcp: lport=%u connected=%d err=%d avail=%u\n",
                lport, tcp_connected(c), tcp_error(c, 0), tcp_rx_avail(c));

    /* Clean up, or the next run starts with one fewer connection. A released
       connection that is not CLOSED waits out an orphan timeout, so push the
       clock well past it and let the timers reap. */
    fuzz_set_inject(NULL);
    tcp_release(c);
    for (int k = 0; k < 6; k++) {
        fuzz_clock_add(ORPHAN_SLACK_MS);
        tcp_timers(timer_ms());
    }
    struct fuzz_frame drain;
    while (fuzz_take(&drain))
        ;
}

/* --- the table ----------------------------------------------------------- */

static const struct fuzz_target table[] = {
    { "wire", t_wire },
    { "ip",   t_ip   },
    { "arp",  t_arp  },
    { "dns",  t_dns  },
    { "tcp",  t_tcp  },
};

const struct fuzz_target *fuzz_targets(unsigned *n)
{
    *n = sizeof table / sizeof table[0];
    return table;
}

/*
 * Resolve the neighbours the targets talk to. ip_send is non-blocking: it
 * parks the frame and sends an ARP request, and the frame only leaves when
 * the reply arrives. A run that has to do that first spends its one packet
 * on ARP and never reaches the code being tested, so this runs before every
 * run. It is idempotent — with the entry already cached, ip_send just
 * transmits and the pump throws the frame away.
 */
static void prime_arp(void)
{
    static const uint8_t pad[8] = { 0 };
    void (*save)(const uint8_t *, uint32_t) = on_ip_frame;
    on_ip_frame = NULL;
    ip_send(PEER_IP, IPPROTO_UDP, pad, sizeof pad);
    pump();
    ip_send(IPV4_DNS, IPPROTO_UDP, pad, sizeof pad);
    pump();
    fuzz_sent_reset();
    on_ip_frame = save;
}

void fuzz_init(void)
{
    ipv4_init();                /* registers on_arp and on_ip through dev_add_pack */
    fuzz_clock_set(1000);
    prime_arp();
}

void fuzz_run(const struct fuzz_target *t, const unsigned char *data, size_t len)
{
    /*
     * The same input should do the same thing every time, or a crash saved at
     * run 200000 cannot be replayed on its own. So the clock is wound back to
     * the same value and the neighbours are re-resolved before each run.
     *
     * It is not perfect: the ARP pending queue and the neighbour table are
     * global and a run can leave a mark on them. If a saved crash does not
     * reproduce under -replay, that is the reason, and the input needs to be
     * run after the one before it.
     */
    fuzz_clock_set(1000);
    fuzz_sent_reset();
    fuzz_set_inject(NULL);
    on_ip_frame = NULL;
    prime_arp();

    t->fn(data, len);

    fuzz_set_inject(NULL);
    on_ip_frame = NULL;
}

/*
 * Between runs the stack should be holding almost nothing: the ARP pending
 * queue is four frames and a released connection is reaped by its timers.
 * A number that climbs means packets are being kept forever, which is a
 * remote denial of service even though nothing crashed. The bound is loose
 * on purpose — this is here to catch a leak, not to pin down a constant.
 */
int fuzz_leaked(void)
{
    return skb_live() > 64;
}
