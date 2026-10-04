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
#include <random.h>
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
    if (src != ipv4_dns || sport != 53 || len < 12 || len > sizeof dns_pkt)
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

#define NAME_MAX  255               /* RFC 1035: the longest name on the wire */
#define CNAME_MAX 8                 /* how far a chain of CNAMEs is followed */

/*
 * Expand the name at `off` into out[], lowercased, so that two names can be
 * compared as plain bytes. Returns its length, or -1.
 *
 * This is the only place a compression pointer is followed rather than
 * stepped over, and following them is exactly how CVE-2020-24338 hung a
 * stack forever: a name that pointed at itself. It is safe here because a
 * pointer must land strictly further back than the last one did. The targets
 * strictly decrease, so a cycle cannot be built at all, and `hops` caps the
 * work so the cost is bounded rather than quadratic in the message length.
 *
 * Lowercasing is not cosmetic: a name is case-insensitive on the wire
 * (RFC 4343), so a server may answer EXAMPLE.com for example.com and a
 * byte compare would reject a perfectly good reply.
 */
static int name_expand(const uint8_t *p, uint32_t len, uint32_t off,
                       uint8_t *out, uint32_t cap)
{
    uint32_t n = 0, limit = off;
    int hops = 0;
    while (off < len) {
        uint8_t l = p[off];
        if ((l & 0xC0) == 0xC0) {               /* a pointer */
            if (off + 1 >= len || ++hops > 8)
                return -1;
            uint32_t to = (uint32_t)(l & 0x3F) << 8 | p[off + 1];
            if (to >= limit)                    /* not strictly backwards */
                return -1;
            limit = to;
            off = to;
            continue;
        }
        if (l & 0xC0)                           /* reserved label type */
            return -1;
        if (l == 0)
            return (int)n;                      /* the root: the name ends */
        if (off + 1 + l >= len || n + 1 + l > cap)
            return -1;
        out[n++] = l;
        for (uint8_t i = 0; i < l; i++) {
            uint8_t c = p[off + 1 + i];
            if (c >= 'A' && c <= 'Z')
                c = (uint8_t)(c + 32);
            out[n++] = c;
        }
        off += 1 + l;
    }
    return -1;                                  /* ran off the end */
}

/*
 * Two buffers for names being compared, static rather than 512 more bytes of
 * kernel stack. Safe because dns_resolve lets one lookup run at a time
 * (dns_busy), which is the same thing that protects dns_pkt and dns_q.
 */
static uint8_t want_name[NAME_MAX + 1];     /* the name we are chasing */
static uint8_t rr_name[NAME_MAX + 1];       /* the name a record is about */

/*
 * The address for the name we asked about.
 *
 * The rule that matters is RFC 5452 §5: an answer is only an answer if it is
 * *about* the name in the question. Without that check a reply which passes
 * the transaction id, the port and the question section can still carry an
 * address for some entirely different name, and we would use it — which is
 * the hole the Kaminsky attack went through.
 *
 * A CNAME makes it awkward, because then the A record is about the end of
 * the chain and not about the question at all. So the chain is walked: a
 * record counts if its owner is the question, or the target of a CNAME that
 * already counted. Records about anything else are skipped, not trusted.
 */
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
    uint32_t first = 12;
    for (uint32_t i = 0; i < qd; i++) {
        int next = skip_name(p, len, first);
        if (next < 0 || (uint32_t)next + 4 > len)
            return -1;
        first = (uint32_t)next + 4;     /* type and class */
    }

    /* Start from the question we sent. It is held uncompressed, name first,
       so the name is everything but the trailing type and class. */
    if (dns_qlen < 5)
        return -1;
    int want_n = name_expand(dns_q, dns_qlen - 4, 0, want_name, sizeof want_name);
    if (want_n <= 0)
        return -1;

    for (int chain = 0; chain < CNAME_MAX; chain++) {
        uint32_t off = first;
        int chased = 0;
        for (uint32_t i = 0; i < an; i++) {
            uint32_t at = off;                  /* where the owner name is */
            int next = skip_name(p, len, off);
            if (next < 0 || (uint32_t)next + 10 > len)
                return -1;
            off = (uint32_t)next;
            uint16_t type  = (uint16_t)((p[off] << 8) | p[off + 1]);
            uint16_t class = (uint16_t)((p[off + 2] << 8) | p[off + 3]);
            uint16_t rdlen = (uint16_t)((p[off + 8] << 8) | p[off + 9]);
            off += 10;
            if (off + rdlen > len)
                return -1;
            uint32_t rdata = off;
            off += rdlen;
            if (class != 1)                     /* IN only */
                continue;
            int rr_n = name_expand(p, len, at, rr_name, sizeof rr_name);
            if (rr_n != want_n || memcmp(rr_name, want_name, (uint32_t)want_n) != 0)
                continue;                       /* about some other name */
            if (type == 1 && rdlen == 4) {
                *ip = ((uint32_t)p[rdata] << 24) | ((uint32_t)p[rdata + 1] << 16) |
                      ((uint32_t)p[rdata + 2] << 8) | p[rdata + 3];
                return 0;
            }
            if (type == 5) {                    /* CNAME: the name moved */
                int t = name_expand(p, len, rdata, want_name, sizeof want_name);
                if (t <= 0)
                    return -1;
                want_n = t;
                chased = 1;
                break;                          /* rescan for the new name */
            }
        }
        if (!chased)
            return -1;                          /* no answer, nothing to follow */
    }
    return -1;                                  /* the chain went on too long */
}

static int dns_lookup(const char *name, uint32_t *ip)
{
    uint8_t q[512];
    memset(q, 0, 12);
    /* The transaction id is 16 bits an off-path spoofer has to guess to
       land a forged answer. As a counter it was one subtraction away from
       free once they had seen a single lookup — that is the arithmetic the
       Kaminsky attack ran on. */
    dns_wait_id = (uint16_t)random_u32();
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
       and it hands an off-path spoofer one less thing to guess. */
    uint16_t port = udp_free_port();
    if (!port)
        return -EADDRINUSE;
    if (udp_bind(port, on_dns, NULL) < 0)
        return -ENOMEM;
    dns_got = 0;
    dns_len = 0;
    /*
     * Three tries, waiting 1s, 2s then 4s -- seven seconds in total, which
     * looks absurd next to QEMU's built-in resolver answering in microseconds
     * from the same machine. It is not. This was two tries of 700ms, and on a
     * tethered link a lookup was measured at 2.1 to 4.3 seconds (the host's
     * own dig reported a 3550ms query time on the same link, with 0% packet
     * loss over 30 pings). A 1.4-second budget against a 3-second answer fails
     * every time, which is what `make test` was doing: -ETIMEDOUT on most runs.
     *
     * Worth being precise about the failure, because the obvious guess is
     * wrong: nothing was lost and nothing was corrupted. The replies were
     * arriving, just later than we were willing to wait -- and from in here a
     * reply that arrives after you have given up looks exactly like one that
     * never came. Only measuring the link told them apart.
     *
     * Growing the wait rather than repeating it matters for the same reason it
     * does in the DHCP client: the question is re-sent with the same id, so a
     * late answer to an earlier try still counts, and the later windows are
     * long enough to catch one. A quick resolver pays nothing for any of this,
     * because the wait ends the moment a reply arrives.
     */
    int rc = -ETIMEDOUT;
    uint32_t wait = 1000;
    for (int try = 0; try < 3 && rc == -ETIMEDOUT; try++, wait *= 2) {
        if (udp_sendto(ipv4_dns, port, 53, q, qlen) < 0) {
            rc = -EIO;
            break;
        }
        uint32_t t0 = timer_ms();
        while (timer_ms() - t0 < wait) {
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
