/*
 * attacks.c — tests that assert an attack is *rejected*.
 *
 * This is the half of the problem fuzzing cannot reach. `make fuzz` proves
 * that 125 million malformed packets do not corrupt memory; it says nothing
 * about whether the stack did the right thing with them, because accepting a
 * forged ARP reply or answering a broadcast ping is not a memory error and
 * no sanitizer will ever complain about it.
 *
 * So each test here performs an actual attack and checks the behaviour that
 * is supposed to stop it. Every one of the fixes in docs/security.md should
 * have an entry, and a fix with no entry here is a fix nobody has tested.
 *
 * Positive controls matter as much as the attacks: a test that asserts "no
 * reply was sent" passes just as well when the stack is completely broken
 * and never replies to anything. So for every "this must be ignored" there
 * is a "this must work" beside it.
 *
 * Run: make attacks
 */

#include <dns.h>
#include <ipv4.h>
#include <netdevice.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tcp.h>
#include <timer.h>
#include <udp.h>

#include "packet.h"
#include "stubs.h"

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO       8

#define EVIL_IP  0x0A00025Au        /* 10.0.2.90, the attacker */
#define FRESH1   0x0A000258u        /* 10.0.2.88, never resolved */
#define FRESH2   0x0A000259u        /* 10.0.2.89 */
#define FRESH3   0x0A00025Bu        /* 10.0.2.91 */
#define FRESH4   0x0A00025Cu        /* 10.0.2.92 */
#define OTHER_IP 0x0A000263u        /* 10.0.2.99, somebody else */
#define BCAST    0xFFFFFFFFu
#define SUBNET_BCAST 0x0A0002FFu    /* 10.0.2.255 */
#define MCAST    0xE0000001u        /* 224.0.0.1 */

static int passed, failed, known_bad, unexpectedly_fixed;

static void ok(int cond, const char *name, const char *why)
{
    if (cond) {
        passed++;
        printf("  pass  %s\n", name);
    } else {
        failed++;
        printf("  FAIL  %s\n          %s\n", name, why);
    }
}

/*
 * A bug that is known and not yet fixed. The test states the behaviour we
 * want, so the day it starts passing the suite says so and the entry can be
 * promoted to a real check. It does not fail the build, because the bug is
 * already written down in docs/security.md. Unused while that list is empty,
 * and kept anyway: the next open bug should be entered this way.
 */
__attribute__((unused))
static void known(int cond, const char *name, const char *note)
{
    if (cond) {
        unexpectedly_fixed++;
        printf("  FIXED %s — now passes; make this a real check\n", name);
    } else {
        known_bad++;
        printf("  known %s\n          %s\n", name, note);
    }
}

/* --- state between tests -------------------------------------------------
   The clock only ever moves forward, by more than the ARP pending timeout,
   so one test can never be looking at a frame another test parked. */

static uint32_t clock_base = 1000;

static void reset(void)
{
    clock_base += 5000;
    fuzz_clock_set(clock_base);
    fuzz_sent_reset();
    fuzz_set_inject(NULL);
    pk_set_ip_sink(NULL);
    pk_prime_arp();
}

/* --- looking at what the stack sent -------------------------------------- */

static unsigned drain(void)
{
    struct fuzz_frame f;
    unsigned n = 0;
    while (fuzz_take(&f))
        n++;
    return n;
}

/* The first frame of Ethernet type `type`, or 0. For IP frames `proto`
   must match too; pass 0 for any. */
static int first_frame(struct fuzz_frame *out, uint16_t type, uint8_t proto)
{
    struct fuzz_frame f;
    int got = 0;
    while (fuzz_take(&f)) {
        if (got || f.len < ETH_HLEN || rd16(f.b + ETH_TYPE) != type)
            continue;
        if (type == ETH_P_IP) {
            if (f.len < ETH_HLEN + IP_HLEN)
                continue;
            if (proto && f.b[ETH_HLEN + IP_PROTO] != proto)
                continue;
        }
        *out = f;
        got = 1;
    }
    return got;
}

/* Which MAC does traffic to `ip` go to? Sends a datagram and reports the
   destination MAC of the frame that carries it. 0 if it did not go out at
   all, which is what happens when the address is not in the cache. */
static int mac_used_for(uint32_t ip, uint8_t out[6])
{
    static const uint8_t pad[8] = { 0 };
    drain();
    ip_send(ip, IPPROTO_UDP, pad, sizeof pad);
    struct fuzz_frame f;
    if (!first_frame(&f, ETH_P_IP, IPPROTO_UDP))
        return 0;
    memcpy(out, f.b + ETH_DST, 6);
    return 1;
}

static int cached_as(uint32_t ip, const uint8_t mac[6])
{
    uint8_t got[6];
    return mac_used_for(ip, got) && memcmp(got, mac, 6) == 0;
}

/* --- ARP ----------------------------------------------------------------- */

static void test_arp(void)
{
    uint8_t arp[ARP_LEN];
    printf("ARP cache\n");

    /* Positive control: a reply to a request we made is believed. */
    reset();
    static const uint8_t pad[8] = { 0 };
    ip_send(FRESH2, IPPROTO_UDP, pad, sizeof pad);      /* now awaited */
    drain();
    pk_arp(arp, 2, peer_mac, FRESH2, OUR_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, peer_mac);
    ok(cached_as(FRESH2, peer_mac), "solicited reply is cached",
       "a reply to our own request was not believed, so nothing can be sent");

    /* The attack: a reply nobody asked for, for an address already known. */
    reset();
    pk_arp(arp, 2, evil_mac, PEER_IP, OUR_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, evil_mac);
    ok(cached_as(PEER_IP, peer_mac), "unsolicited reply cannot overwrite the cache",
       "an unsolicited ARP reply redirected the gateway: traffic now goes to the attacker");

    /* The same attack against an address we have never looked up. */
    reset();
    pk_arp(arp, 2, evil_mac, FRESH1, OUR_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, evil_mac);
    uint8_t got[6];
    ok(!mac_used_for(FRESH1, got), "unsolicited reply cannot create an entry",
       "an unsolicited ARP reply put an address in the cache that was never requested");

    /* A reply for us is only believed from a sender MAC that could be real. */
    reset();
    ip_send(FRESH3, IPPROTO_UDP, pad, sizeof pad);
    drain();
    pk_arp(arp, 2, (const uint8_t[]){ 0x01, 0x00, 0x5E, 0, 0, 1 }, FRESH3, OUR_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, peer_mac);
    ok(!mac_used_for(FRESH3, got), "reply with a multicast sender MAC is refused",
       "a multicast MAC was cached: every packet to that host would be flooded");

    reset();
    ip_send(FRESH4, IPPROTO_UDP, pad, sizeof pad);
    drain();
    pk_arp(arp, 2, (const uint8_t[]){ 0, 0, 0, 0, 0, 0 }, FRESH4, OUR_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, peer_mac);
    ok(!mac_used_for(FRESH4, got), "reply with an all-zero sender MAC is refused",
       "an all-zero MAC was cached");

    /* We answer a request for our address... */
    reset();
    pk_arp(arp, 1, evil_mac, EVIL_IP, OUR_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, evil_mac);
    struct fuzz_frame f;
    int replied = first_frame(&f, ETH_P_ARP, 0) &&
                  rd16(f.b + ETH_HLEN + ARP_OPER) == 2;
    ok(replied, "a request for our address is answered",
       "we did not answer an ARP request for our own address: nobody can reach us");

    /* ...without learning anything from it. A request is an attacker's way
       of putting an entry in the cache for free, and Linux does cache it. */
    reset();
    pk_arp(arp, 1, evil_mac, EVIL_IP, OUR_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, evil_mac);
    drain();
    ok(!mac_used_for(EVIL_IP, got), "a request teaches the cache nothing",
       "an ARP request created a cache entry: an attacker can seed the cache unasked");

    /* A request for somebody else is not ours to answer. */
    reset();
    pk_arp(arp, 1, evil_mac, EVIL_IP, OTHER_IP);
    pk_feed(ETH_P_ARP, arp, sizeof arp, evil_mac);
    ok(drain() == 0, "a request for another address is ignored",
       "we answered an ARP request for an address that is not ours");
}

/* --- ICMP and reflection ------------------------------------------------- */

static uint32_t echo_pkt(uint8_t *out, uint32_t cap, uint8_t type, uint32_t body_n)
{
    uint8_t icmp[1600];
    if (body_n + 8 > sizeof icmp)
        return 0;
    memset(icmp, 0, 8);
    icmp[ICMP_TYPE] = type;
    wr16(icmp + ICMP_ID, 0x4242);
    wr16(icmp + ICMP_SEQ, 1);
    for (uint32_t i = 0; i < body_n; i++)
        icmp[8 + i] = (uint8_t)('a' + i % 26);
    wr16(icmp + ICMP_CHECK, pk_csum(icmp, 8 + body_n));
    return pk_ip(out, cap, IPPROTO_ICMP, icmp, 8 + body_n);
}

static int echo_was_answered(uint32_t dst, uint32_t body_n)
{
    uint8_t pkt[1700];
    uint32_t n = echo_pkt(pkt, sizeof pkt, ICMP_ECHO, body_n);
    if (dst != OUR_IP)
        pk_ip_set_dst(pkt, dst);
    pk_feed(ETH_P_IP, pkt, n, peer_mac);
    struct fuzz_frame f;
    return first_frame(&f, ETH_P_IP, IPPROTO_ICMP) &&
           f.b[ETH_HLEN + IP_HLEN + ICMP_TYPE] == ICMP_ECHO_REPLY;
}

static void test_icmp(void)
{
    printf("ICMP reflection\n");

    reset();
    ok(echo_was_answered(OUR_IP, 8), "an echo to our address is answered",
       "ping to this machine does not work at all");

    /* Answering a broadcast ping is what makes a machine a smurf amplifier:
       one spoofed packet, every host on the segment replies to the victim. */
    reset();
    ok(!echo_was_answered(BCAST, 8), "a broadcast echo is not answered",
       "this machine is a smurf amplifier: it answers pings sent to the broadcast address");

    reset();
    ok(!echo_was_answered(SUBNET_BCAST, 8), "a subnet broadcast echo is not answered",
       "this machine answers pings sent to the subnet broadcast address");

    reset();
    ok(!echo_was_answered(MCAST, 8), "a multicast echo is not answered",
       "this machine answers pings sent to a multicast group");

    reset();
    ok(!echo_was_answered(OTHER_IP, 8), "an echo for another address is not answered",
       "we answered a ping addressed to a different machine");

    /* The reflection buffer is 1400 bytes and the guard is the only thing
       between a 1460-byte echo and a 60-byte overflow of a static array. */
    reset();
    ok(echo_was_answered(OUR_IP, 1300), "a large echo within the buffer is answered",
       "a legitimate large ping was dropped");

    reset();
    ok(!echo_was_answered(OUR_IP, 1460), "an echo too big for the buffer is not reflected",
       "an echo larger than the reply buffer was reflected: that is a buffer overflow");

    /* A fragment cannot be checked without reassembling it, so it is dropped. */
    reset();
    uint8_t pkt[1700];
    uint32_t n = echo_pkt(pkt, sizeof pkt, ICMP_ECHO, 8);
    wr16(pkt + IP_FRAG, 0x2000);                /* more-fragments */
    wr16(pkt + IP_CHECK, 0);
    wr16(pkt + IP_CHECK, pk_csum(pkt, IP_HLEN));
    pk_feed(ETH_P_IP, pkt, n, peer_mac);
    ok(drain() == 0, "a fragment is dropped", "a fragmented packet was processed");
}

/* --- our own ping, and who is allowed to answer it ---------------------- */

static uint32_t ping_from;              /* who the fake reply comes from */
static int ping_replies;

static void ping_sink(const uint8_t *ip, uint32_t len)
{
    if (len < IP_HLEN + 8 || ip[IP_PROTO] != IPPROTO_ICMP)
        return;
    if (ip[IP_HLEN + ICMP_TYPE] != ICMP_ECHO)
        return;
    ping_replies++;
    /* Echo it back, with the id and sequence it chose, from ping_from. */
    uint8_t icmp[64];
    uint32_t body = len - IP_HLEN;
    if (body > sizeof icmp)
        body = sizeof icmp;
    memcpy(icmp, ip + IP_HLEN, body);
    icmp[ICMP_TYPE] = ICMP_ECHO_REPLY;
    wr16(icmp + ICMP_CHECK, 0);
    wr16(icmp + ICMP_CHECK, pk_csum(icmp, body));
    uint8_t pkt[256];
    uint32_t n = pk_ip(pkt, sizeof pkt, IPPROTO_ICMP, icmp, body);
    pk_ip_set_src(pkt, ping_from);
    pk_feed(ETH_P_IP, pkt, n, peer_mac);
}

static void test_ping(void)
{
    printf("our own ping\n");

    reset();
    ping_from = PEER_IP;
    ping_replies = 0;
    pk_set_ip_sink(ping_sink);
    fuzz_set_inject(pk_pump);
    int rc = net_ping(PEER_IP);
    fuzz_set_inject(NULL);
    pk_set_ip_sink(NULL);
    ok(rc == 0, "a ping answered by the host we pinged succeeds",
       "ping does not work even when the right host replies");

    /* The reply carries the right id and sequence but comes from somebody
       else. Without a source check, any host on the segment can confirm
       reachability of a host that is actually down. */
    reset();
    ping_from = OTHER_IP;
    ping_replies = 0;
    pk_set_ip_sink(ping_sink);
    fuzz_set_inject(pk_pump);
    rc = net_ping(PEER_IP);
    fuzz_set_inject(NULL);
    pk_set_ip_sink(NULL);
    ok(rc != 0 && ping_replies > 0, "a ping reply from the wrong host is ignored",
       "an echo reply from a host we did not ping satisfied our ping");
}

/* --- UDP ----------------------------------------------------------------- */

static void test_udp(void)
{
    printf("UDP\n");

    /* No ICMP port-unreachable. It would be correct per RFC 792 and it would
       also be a reflector: one spoofed packet, one packet at the victim. */
    reset();
    uint8_t udp[8];
    wr16(udp, 1234);
    wr16(udp + 2, 9999);            /* nothing is bound here */
    wr16(udp + 4, 8);
    wr16(udp + 6, 0);               /* no checksum */
    uint8_t pkt[64];
    uint32_t n = pk_ip(pkt, sizeof pkt, IPPROTO_UDP, udp, sizeof udp);
    pk_feed(ETH_P_IP, pkt, n, peer_mac);
    ok(drain() == 0, "a datagram to an unbound port draws no reply",
       "we answered a datagram to a closed port: that is a reflector");

    reset();
    wr16(udp + 2, 0);               /* port 0 is not a port */
    n = pk_ip(pkt, sizeof pkt, IPPROTO_UDP, udp, sizeof udp);
    pk_feed(ETH_P_IP, pkt, n, peer_mac);
    ok(drain() == 0, "a datagram to port 0 draws no reply", "we answered a datagram to port 0");
}

/* --- TCP ----------------------------------------------------------------- */

static uint32_t t_our_seq, t_our_len, t_their_seq;
static uint16_t t_window = 0xFFFF;  /* the window the peer advertises */
static unsigned t_payloads;         /* segments we sent that carried data */
static uint32_t t_payload_n;        /* how many bytes the last one carried */

static void tcp_sink(const uint8_t *ip, uint32_t len)
{
    if (len < IP_HLEN + 20 || ip[IP_PROTO] != IPPROTO_TCP)
        return;
    const uint8_t *t = ip + IP_HLEN;
    uint32_t hl = (uint32_t)(t[12] >> 4) * 4;
    uint32_t total = rd16(ip + IP_TOT_LEN);
    t_our_seq = rd32(t + 4);
    t_our_len = (hl < 20 || total < IP_HLEN + hl) ? 0 : total - IP_HLEN - hl;
    if (t_our_len) {
        t_payloads++;
        t_payload_n = t_our_len;
    }
    if (t[13] & 0x03)                           /* SYN and FIN each take one */
        t_our_len += 1;
    t_their_seq = rd32(t + 8);
}

static void seg_send(uint16_t lport, uint8_t flags, uint32_t seq, uint32_t ack,
                     const uint8_t *data, uint32_t data_n)
{
    uint8_t seg[1500];
    if (20 + data_n > sizeof seg)
        return;
    memset(seg, 0, 20);
    wr16(seg, PEER_PORT);
    wr16(seg + 2, lport);
    wr32(seg + 4, seq);
    wr32(seg + 8, ack);
    seg[12] = 5 << 4;
    seg[13] = flags;
    wr16(seg + 14, t_window);
    if (data_n)
        memcpy(seg + 20, data, data_n);
    wr16(seg + 16, 0);
    wr16(seg + 16, ip_transport_csum(PEER_IP, OUR_IP, IPPROTO_TCP, seg, 20 + data_n));
    tcp_input(PEER_IP, OUR_IP, seg, 20 + data_n);
    /* Deliberately does NOT pump: the pump drains the send queue, and a test
       that wants to look at what the stack replied has to look first. */
}

/* A connection in ESTABLISHED, with the sequence numbers both sides expect. */
static struct tcp_conn *establish(uint16_t *lport)
{
    t_our_seq = t_our_len = t_their_seq = 0;
    t_payloads = t_payload_n = 0;
    t_window = 0xFFFF;
    pk_set_ip_sink(tcp_sink);
    fuzz_set_inject(pk_pump);
    struct tcp_conn *c = tcp_open();
    if (!c)
        return NULL;
    tcp_connect(c, PEER_IP, PEER_PORT, 1);
    pk_pump();
    uint32_t raddr = 0;
    uint16_t rport = 0;
    tcp_names(c, lport, &raddr, &rport);
    seg_send(*lport, 0x12 /* SYN|ACK */, t_their_seq, t_our_seq + t_our_len, NULL, 0);
    pk_pump();                  /* record the ACK, so t_their_seq is current */
    return c;
}

static void finish(struct tcp_conn *c)
{
    fuzz_set_inject(NULL);
    pk_set_ip_sink(NULL);
    if (c)
        tcp_release(c);
    for (int k = 0; k < 6; k++) {
        fuzz_clock_add(30000);
        tcp_timers(timer_ms());
    }
    drain();
}

static void test_tcp(void)
{
    uint16_t lport = 0;
    printf("TCP\n");

    reset();
    struct tcp_conn *c = establish(&lport);
    ok(c && tcp_connected(c), "the handshake completes",
       "a connection cannot be established at all, so every test below is meaningless");
    finish(c);

    /* RFC 5961: a reset must land exactly on the next expected sequence.
       Otherwise an off-path attacker who can guess the ports kills any
       connection by spraying resets across the sequence space. */
    reset();
    c = establish(&lport);
    if (c && tcp_connected(c)) {
        seg_send(lport, 0x04 /* RST */, t_their_seq + 70000, 0, NULL, 0);
        ok(tcp_connected(c), "an out-of-window reset is ignored",
           "a blind reset tore down the connection: anyone who guesses the ports can kill it");
    } else {
        ok(0, "an out-of-window reset is ignored", "could not establish a connection");
    }
    finish(c);

    /* Positive control: the reset that is actually legitimate works. */
    reset();
    c = establish(&lport);
    if (c && tcp_connected(c)) {
        seg_send(lport, 0x04, t_their_seq, 0, NULL, 0);
        ok(!tcp_connected(c) || tcp_error(c, 0) != 0, "an in-window reset is accepted",
           "a legitimate reset was ignored, so a dead connection never closes");
    } else {
        ok(0, "an in-window reset is accepted", "could not establish a connection");
    }
    finish(c);

    /* The window tests below all shut the peer's window with an ack that
       acknowledges nothing new, then queue data behind it. The sequence the
       probe goes out at is still snd_una, because a peer whose window is shut
       must drop a probe that is past it: nothing we send advances the ack. */
    uint32_t una;
    static uint8_t queued[200];

    /* RFC 1122 4.2.2.17. A zero window is an ordinary thing for a peer to
       advertise — its application has stopped reading — and it may hold it
       there. Nothing can be sent, so without a probe no timer is left running
       and the connection stops dead with the data still queued. */
    reset();
    c = establish(&lport);
    if (c && tcp_connected(c)) {
        t_window = 0;
        seg_send(lport, 0x10 /* ACK */, t_their_seq, t_our_seq + t_our_len, NULL, 0);
        pk_pump();
        t_payloads = 0;
        tcp_write(c, queued, sizeof queued, 0, 1 /* nonblock */);
        pk_pump();
        int probed = t_payloads >= 1 && t_payload_n == 1;
        unsigned first = t_payloads;
        for (int k = 0; k < 4; k++) {   /* each step is past the longest backoff */
            fuzz_clock_add(70000);
            tcp_timers(timer_ms());
            pk_pump();
        }
        ok(probed && t_payloads > first, "a shut window is probed, not waited on",
           "a peer advertising a zero window stalled the connection: no probe went out, "
           "so a lost window update is never noticed and the data never moves");
    } else {
        ok(0, "a shut window is probed, not waited on", "could not establish a connection");
    }
    finish(c);

    /* And the probing is bounded. RFC 1122 says to keep probing for as long as
       the peer answers, but a peer that advertises zero and then says nothing
       at all must not hold a connection and its buffer open forever. */
    reset();
    c = establish(&lport);
    if (c && tcp_connected(c)) {
        t_window = 0;
        seg_send(lport, 0x10, t_their_seq, t_our_seq + t_our_len, NULL, 0);
        pk_pump();
        tcp_write(c, queued, sizeof queued, 0, 1);
        pk_pump();
        for (int k = 0; k < 14; k++) {  /* comfortably more than PROBES */
            fuzz_clock_add(70000);
            tcp_timers(timer_ms());
            drain();
        }
        ok(!tcp_connected(c) || tcp_error(c, 0) != 0, "a window shut and gone silent gives up",
           "a peer that advertised a zero window and then stopped answering kept the "
           "connection alive indefinitely");
    } else {
        ok(0, "a window shut and gone silent gives up", "could not establish a connection");
    }
    finish(c);

    /* Positive control: the window reopening has to actually deliver. The
       update acknowledges nothing new, which is the honest case — the peer
       dropped the probe byte because it was past the window. */
    reset();
    c = establish(&lport);
    if (c && tcp_connected(c)) {
        t_window = 0;
        seg_send(lport, 0x10, t_their_seq, t_our_seq + t_our_len, NULL, 0);
        pk_pump();
        tcp_write(c, queued, sizeof queued, 0, 1);
        pk_pump();
        una = t_our_seq;
        t_window = 0xFFFF;
        t_payloads = 0;
        seg_send(lport, 0x10, t_their_seq, una, NULL, 0);
        pk_pump();
        ok(t_payloads >= 1 && t_payload_n > 1, "a window that reopens delivers the data",
           "the window reopened and the queued data stayed queued");
    } else {
        ok(0, "a window that reopens delivers the data", "could not establish a connection");
    }
    finish(c);

    /* The probes must not be charged against the connection once the window
       is open again: a long wait on a shut window is not evidence of a lost
       packet, and treating it as such kills a connection that just got well. */
    reset();
    c = establish(&lport);
    if (c && tcp_connected(c)) {
        t_window = 0;
        seg_send(lport, 0x10, t_their_seq, t_our_seq + t_our_len, NULL, 0);
        pk_pump();
        tcp_write(c, queued, sizeof queued, 0, 1);
        pk_pump();
        una = t_our_seq;
        for (int k = 0; k < 8; k++) {   /* probe nearly to the limit */
            fuzz_clock_add(70000);
            tcp_timers(timer_ms());
            drain();
        }
        t_window = 0xFFFF;
        seg_send(lport, 0x10, t_their_seq, una, NULL, 0);
        pk_pump();                      /* the queued data is allowed out */
        t_payloads = 0;
        fuzz_clock_add(5000);           /* longer than any ordinary interval */
        tcp_timers(timer_ms());
        pk_pump();
        int prompt = t_payloads >= 1;   /* the unacked data was retried */
        for (int k = 0; k < 2; k++) {   /* and the probes are not held against it */
            fuzz_clock_add(65000);
            tcp_timers(timer_ms());
            drain();
        }
        ok(prompt && tcp_connected(c) && tcp_error(c, 0) == 0,
           "a reopened window clears the probe backoff",
           "the long backoff and the probe count survived the window reopening, so "
           "recovery crawled and the next timer killed a connection that was healthy");
    } else {
        ok(0, "a reopened window clears the probe backoff", "could not establish a connection");
    }
    finish(c);

    /* Data arriving for a connection that does not exist gets a reset. That
       is correct, and it is also an unlimited reflector — see the rate
       limiting entry in docs/security.md. */
    reset();
    seg_send(40000 /* nothing is listening */, 0x18, 1000, 2000, (const uint8_t *)"x", 1);
    struct fuzz_frame f;
    int rst = first_frame(&f, ETH_P_IP, IPPROTO_TCP) &&
              (f.b[ETH_HLEN + IP_HLEN + 13] & 0x04);
    ok(rst, "data for an unknown connection draws a reset",
       "we silently ignored data for a closed port instead of resetting it");
    drain();
}

/* --- DNS ----------------------------------------------------------------- */

#define GOOD_ADDR 0x68141796u       /* 104.20.23.150, the real answer */
#define EVIL_ADDR 0x0A0002FEu       /* 10.0.2.254, the attacker's server */

struct inject {
    int      wrong_id;
    int      wrong_question;
    int      self_ptr;          /* owner name is a pointer to itself */
    uint32_t src;
    uint16_t sport;
    uint16_t ancount;
    const uint8_t *answer;
    uint32_t answer_n;
};

static struct inject inj;
static int inj_done;

static void dns_sink(const uint8_t *ip, uint32_t len)
{
    if (inj_done || len < IP_HLEN + 8 || ip[IP_PROTO] != IPPROTO_UDP)
        return;
    const uint8_t *udp = ip + IP_HLEN;
    if (rd16(udp + 2) != 53)
        return;
    uint16_t our_port = rd16(udp);
    const uint8_t *q = udp + 8;
    uint32_t qn = len - IP_HLEN - 8;
    if (qn < 12)
        return;
    inj_done = 1;

    uint8_t pkt[8 + 512];
    uint32_t qlen = qn - 12;
    if (qlen + 20 + inj.answer_n > sizeof pkt)
        return;
    uint32_t o = 8;
    memset(pkt + o, 0, 12);
    wr16(pkt + o, inj.wrong_id ? (uint16_t)(rd16(q) ^ 0xFFFF) : rd16(q));
    wr16(pkt + o + 2, 0x8180);              /* response, recursion available */
    wr16(pkt + o + 4, 1);                   /* one question */
    wr16(pkt + o + 6, inj.ancount);
    o += 12;
    memcpy(pkt + o, q + 12, qlen);
    if (inj.wrong_question)
        pkt[o + 1] ^= 0x20;                 /* one letter of the name */
    o += qlen;
    if (inj.self_ptr) {
        /* An A record whose owner name is a pointer to where that pointer
           itself sits. A parser that resolves names by following pointers
           without a rule about direction never comes back from this. */
        uint32_t here = o - 8;          /* offset within the DNS message */
        wr16(pkt + o, (uint16_t)(0xC000 | here)); o += 2;
        wr16(pkt + o, 1);  o += 2;      /* type A */
        wr16(pkt + o, 1);  o += 2;      /* class IN */
        wr32(pkt + o, 60); o += 4;      /* ttl */
        wr16(pkt + o, 4);  o += 2;      /* rdlength */
        wr32(pkt + o, EVIL_ADDR); o += 4;
    } else if (inj.answer_n) {
        memcpy(pkt + o, inj.answer, inj.answer_n);
        o += inj.answer_n;
    }
    wr16(pkt, inj.sport);
    wr16(pkt + 2, our_port);
    wr16(pkt + 4, (uint16_t)o);
    wr16(pkt + 6, 0);
    udp_input(inj.src, OUR_IP, pkt, o);
}

/* An A record. `owner` is the compressed name it is about: 0xC00C points at
   the question, anything else is a name of our choosing. */
static uint32_t a_record(uint8_t *out, const uint8_t *owner, uint32_t owner_n,
                         uint32_t addr)
{
    uint32_t o = 0;
    memcpy(out, owner, owner_n);
    o += owner_n;
    wr16(out + o, 1);  o += 2;              /* type A */
    wr16(out + o, 1);  o += 2;              /* class IN */
    wr32(out + o, 60); o += 4;              /* ttl */
    wr16(out + o, 4);  o += 2;              /* rdlength */
    wr32(out + o, addr); o += 4;
    return o;
}

static uint32_t cname_record(uint8_t *out, const uint8_t *owner, uint32_t owner_n,
                             const uint8_t *target, uint32_t target_n)
{
    uint32_t o = 0;
    memcpy(out, owner, owner_n);
    o += owner_n;
    wr16(out + o, 5);  o += 2;              /* type CNAME */
    wr16(out + o, 1);  o += 2;              /* class IN */
    wr32(out + o, 60); o += 4;              /* ttl */
    wr16(out + o, (uint16_t)target_n); o += 2;
    memcpy(out + o, target, target_n);
    o += target_n;
    return o;
}

static int resolve_with(struct inject in, uint32_t *ip)
{
    inj = in;
    inj_done = 0;
    *ip = 0;
    pk_set_ip_sink(dns_sink);
    fuzz_set_inject(pk_pump);
    int rc = dns_resolve("example.com", ip);
    fuzz_set_inject(NULL);
    pk_set_ip_sink(NULL);
    return rc;
}

static void test_dns(void)
{
    printf("DNS\n");
    static const uint8_t ptr_q[2] = { 0xC0, 0x0C };     /* -> the question */
    uint8_t ans[128];
    uint32_t ans_n = a_record(ans, ptr_q, 2, GOOD_ADDR);
    uint32_t ip = 0;

    struct inject base = { .src = IPV4_DNS, .sport = 53, .ancount = 1,
                           .answer = ans, .answer_n = ans_n };

    reset();
    ok(resolve_with(base, &ip) == 0 && ip == GOOD_ADDR,
       "a well-formed reply resolves", "DNS does not work at all");

    /* The transaction id is the only thing an off-path spoofer must guess
       besides the port. This is the Kaminsky precondition. */
    reset();
    struct inject bad = base;
    bad.wrong_id = 1;
    ok(resolve_with(bad, &ip) != 0, "a reply with the wrong transaction id is refused",
       "a reply with a transaction id we never sent was accepted");

    reset();
    bad = base;
    bad.wrong_question = 1;
    ok(resolve_with(bad, &ip) != 0, "a reply whose question differs is refused",
       "a reply echoing a different question was accepted");

    reset();
    bad = base;
    bad.src = EVIL_IP;
    ok(resolve_with(bad, &ip) != 0, "a reply from the wrong address is refused",
       "a reply from a host that is not our resolver was accepted");

    reset();
    bad = base;
    bad.sport = 54;
    ok(resolve_with(bad, &ip) != 0, "a reply from the wrong port is refused",
       "a reply from a source port other than 53 was accepted");

    reset();
    bad = base;
    bad.ancount = 0;
    ok(resolve_with(bad, &ip) != 0, "a reply with no answers fails",
       "a reply claiming no answers still produced an address");

    /*
     * RFC 5452 §5 response matching. The record is a perfectly valid A
     * record about a name nobody asked about. Accepting it is the hole the
     * Kaminsky attack went through, and it is not a memory error, so no
     * amount of fuzzing would ever have found it.
     */
    static const uint8_t evil_name[] = { 4, 'e', 'v', 'i', 'l', 2, 'i', 'o', 0 };
    static const uint8_t qname[] = { 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
                                     3, 'c', 'o', 'm', 0 };
    static const uint8_t qname_upper[] = { 7, 'E', 'X', 'A', 'M', 'P', 'L', 'E',
                                           3, 'C', 'O', 'M', 0 };
    static const uint8_t wname[] = { 3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p',
                                     'l', 'e', 3, 'c', 'o', 'm', 0 };
    uint8_t rr[256];

    reset();
    uint32_t n = a_record(rr, evil_name, sizeof evil_name, EVIL_ADDR);
    bad = base;
    bad.answer = rr;
    bad.answer_n = n;
    ok(resolve_with(bad, &ip) != 0 && ip != EVIL_ADDR,
       "an answer about a different name is refused",
       "an A record about evil.io was returned as the address of example.com");

    /* And the record we did ask for is still found when it is not first:
       a foreign record must be skipped, not treated as the end of the list. */
    reset();
    n = a_record(rr, evil_name, sizeof evil_name, EVIL_ADDR);
    n += a_record(rr + n, qname, sizeof qname, GOOD_ADDR);
    bad = base;
    bad.answer = rr;
    bad.answer_n = n;
    bad.ancount = 2;
    ok(resolve_with(bad, &ip) == 0 && ip == GOOD_ADDR,
       "a foreign record is skipped, not fatal",
       "a record about another name stopped the search and the real answer was missed");

    /* A name on the wire ignores case (RFC 4343), so a server is free to
       answer EXAMPLE.COM. Rejecting that would break real lookups. */
    reset();
    n = a_record(rr, qname_upper, sizeof qname_upper, GOOD_ADDR);
    bad = base;
    bad.answer = rr;
    bad.answer_n = n;
    ok(resolve_with(bad, &ip) == 0 && ip == GOOD_ADDR,
       "an answer differing only in case is accepted",
       "a reply that spelled the name in capitals was rejected");

    /* The reason response matching could not simply require owner == question:
       with a CNAME the A record is about the end of the chain. This is the
       regression guard for that, and it is the test most likely to catch a
       future tightening of the rule that breaks real DNS. */
    reset();
    n = cname_record(rr, ptr_q, 2, wname, sizeof wname);
    n += a_record(rr + n, wname, sizeof wname, GOOD_ADDR);
    bad = base;
    bad.answer = rr;
    bad.answer_n = n;
    bad.ancount = 2;
    ok(resolve_with(bad, &ip) == 0 && ip == GOOD_ADDR,
       "a CNAME chain is followed",
       "a CNAME could not be followed, so a large share of real names will not resolve");

    /* A CNAME pointing back at its own owner. The chain limit is the only
       thing between this and a loop. */
    reset();
    n = cname_record(rr, ptr_q, 2, wname, sizeof wname);
    n += cname_record(rr + n, wname, sizeof wname, qname, sizeof qname);
    bad = base;
    bad.answer = rr;
    bad.answer_n = n;
    bad.ancount = 2;
    ok(resolve_with(bad, &ip) != 0, "a CNAME loop terminates and fails",
       "a CNAME that points back at the question did not terminate cleanly");

    /* CVE-2020-24338: a name that points at itself. Reaching this line at
       all is most of the test — a parser that follows pointers without a
       direction rule never returns. */
    reset();
    bad = base;
    bad.self_ptr = 1;
    bad.answer = NULL;
    bad.answer_n = 0;
    ok(resolve_with(bad, &ip) != 0 && ip != EVIL_ADDR,
       "a name pointing at itself is refused",
       "a self-referential compression pointer was resolved instead of refused");
}

int main(void)
{
    ipv4_init();
    fuzz_clock_set(clock_base);
    pk_prime_arp();

    printf("\nAdversarial tests for the network stack\n\n");
    test_arp();
    test_icmp();
    test_ping();
    test_udp();
    test_tcp();
    test_dns();

    printf("\n%d passed, %d failed", passed, failed);
    if (known_bad)
        printf(", %d known bug%s still open", known_bad, known_bad == 1 ? "" : "s");
    if (unexpectedly_fixed)
        printf(", %d now fixed", unexpectedly_fixed);
    printf("\n");
    if (failed) {
        printf("ATTACKS FAILED\n");
        return 1;
    }
    printf("ATTACKS OK\n");
    return 0;
}
