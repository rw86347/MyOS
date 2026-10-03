/*
 * dns.c — one A-record lookup (step 10)
 *
 * A query is a short UDP datagram to port 53: an id, the "please recurse"
 * bit, and the name split into length-prefixed labels ("example.com" is
 * 7,'e','x',... 3,'c','o','m', 0). The reply uses the same id. Names in
 * the reply may be compressed (a pointer instead of the text); skipping a
 * name only has to step over those bytes, not follow the pointer, so a
 * pointer loop cannot spin here. We take the first IPv4 address.
 */
#include <dns.h>
#include <errno.h>
#include <interrupts.h>
#include <ipv4.h>
#include <netdevice.h>
#include <string.h>
#include <timer.h>
#include <udp.h>

static volatile int dns_got;
static int dns_busy;                /* one lookup at a time: the state below is shared */
static uint16_t dns_wait_id;
static uint8_t dns_pkt[512];
static uint32_t dns_len;
static uint8_t dns_q[260];          /* the question we asked: name, type, class */
static uint32_t dns_qlen;

static void on_dns(void *ctx, uint32_t src, uint16_t sport, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    if (src != IPV4_DNS || sport != 53 || len < 12 || len > sizeof dns_pkt)
        return;
    if ((uint16_t)((data[0] << 8) | data[1]) != dns_wait_id)
        return;
    /* One question, and the same one we asked. A server echoes the question
       section back verbatim, so this is a byte compare. Without it an answer
       about any name at all would be taken as the answer about this one. */
    if (((data[4] << 8) | data[5]) != 1)
        return;
    if (len < 12 + dns_qlen || memcmp(data + 12, dns_q, dns_qlen) != 0)
        return;
    memcpy(dns_pkt, data, len);
    dns_len = len;
    dns_got = 1;
}

static int encode_name(uint8_t *dst, uint32_t cap, const char *name)
{
    uint32_t n = 0;
    if (!name || !name[0] || strlen(name) > 253)
        return -1;
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.')
            dot++;
        uint32_t lab = (uint32_t)(dot - name);
        if (lab == 0 || lab > 63 || n + 1 + lab + 1 > cap)
            return -1;
        dst[n++] = (uint8_t)lab;
        memcpy(dst + n, name, lab);
        n += lab;
        name = *dot == '.' ? dot + 1 : dot;
    }
    dst[n++] = 0;
    return (int)n;
}

/* Byte offset just past the name that starts at off, or -1. */
static int skip_name(const uint8_t *p, uint32_t len, uint32_t off)
{
    int steps = 0;
    while (off < len && steps++ < 64) {
        uint8_t l = p[off];
        if (l == 0)
            return (int)(off + 1);
        if ((l & 0xC0) == 0xC0)
            return off + 1 < len ? (int)(off + 2) : -1;
        if (l & 0xC0)
            return -1;
        if (off + 1 + l >= len)
            return -1;
        off += 1 + l;
    }
    return -1;
}

static int parse_a(const uint8_t *p, uint32_t len, uint32_t *ip)
{
    if (len < 12)
        return -1;
    if ((p[2] & 0x80) == 0)
        return -1;                     /* not a response */
    if ((p[3] & 0x0F) != 0)
        return -1;                     /* rcode: no address */
    uint32_t qd = (uint32_t)((p[4] << 8) | p[5]);
    uint32_t an = (uint32_t)((p[6] << 8) | p[7]);
    if (qd > 4 || an > 16)
        return -1;
    uint32_t off = 12;
    for (uint32_t i = 0; i < qd; i++) {
        int next = skip_name(p, len, off);
        if (next < 0 || (uint32_t)next + 4 > len)
            return -1;
        off = (uint32_t)next + 4;       /* type and class */
    }
    for (uint32_t i = 0; i < an; i++) {
        int next = skip_name(p, len, off);
        if (next < 0 || (uint32_t)next + 10 > len)
            return -1;
        off = (uint32_t)next;
        uint16_t type = (uint16_t)((p[off] << 8) | p[off + 1]);
        uint16_t class = (uint16_t)((p[off + 2] << 8) | p[off + 3]);
        uint16_t rdlen = (uint16_t)((p[off + 8] << 8) | p[off + 9]);
        off += 10;
        if (off + rdlen > len)
            return -1;
        if (type == 1 && class == 1 && rdlen == 4) {
            *ip = ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
                  ((uint32_t)p[off + 2] << 8) | p[off + 3];
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

static int dns_lookup(const char *name, uint32_t *ip)
{
    uint8_t q[512];
    memset(q, 0, 12);
    dns_wait_id++;
    if (dns_wait_id == 0)
        dns_wait_id = 1;
    q[0] = (uint8_t)(dns_wait_id >> 8);
    q[1] = (uint8_t)dns_wait_id;
    q[2] = 0x01;                        /* recursion desired */
    q[5] = 1;                           /* one question */
    int nlen = encode_name(q + 12, sizeof q - 16, name);
    if (nlen < 0)
        return -EINVAL;
    uint32_t qlen = 12 + (uint32_t)nlen;
    q[qlen++] = 0;
    q[qlen++] = 1;                      /* type A */
    q[qlen++] = 0;
    q[qlen++] = 1;                      /* class IN */

    /* Remember the question so a reply can be matched against it. */
    dns_qlen = qlen - 12;
    if (dns_qlen > sizeof dns_q)
        return -EINVAL;
    memcpy(dns_q, q + 12, dns_qlen);

    /* A fresh ephemeral port per lookup, not a fixed one: a fixed port lets
       any local program bind it and break every lookup the kernel makes,
       and it hands an off-path spoofer one less thing to guess. The
       transaction id is still a counter — that needs the CSPRNG, see
       docs/gaps.md. */
    uint16_t port = udp_free_port();
    if (!port)
        return -EADDRINUSE;
    if (udp_bind(port, on_dns, NULL) < 0)
        return -ENOMEM;
    dns_got = 0;
    dns_len = 0;
    int rc = -ETIMEDOUT;
    for (int try = 0; try < 2 && rc == -ETIMEDOUT; try++) {
        if (udp_sendto(IPV4_DNS, port, 53, q, qlen) < 0) {
            rc = -EIO;
            break;
        }
        uint32_t t0 = timer_ms();
        while (timer_ms() - t0 < 700) {
            net_rx_action();
            if (dns_got) {
                rc = parse_a(dns_pkt, dns_len, ip) == 0 ? 0 : -EIO;
                break;
            }
            net_sleep(10);
        }
    }
    udp_unbind(port);
    return rc;
}

int dns_resolve(const char *name, uint32_t *ip)
{
    if (!ip)
        return -EINVAL;
    /* One lookup at a time. The reply state is a single set of globals, and
       two callers would swap answers — which is a security bug, not just a
       correctness one. Nothing sleeps between the test and the set, and
       syscalls run with interrupts off, so this needs no lock. */
    if (dns_busy)
        return -EBUSY;
    dns_busy = 1;
    int rc = dns_lookup(name, ip);
    dns_busy = 0;
    return rc;
}
