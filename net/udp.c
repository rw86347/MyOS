/*
 * udp.c — UDP (step 10)
 *
 * A datagram is one IP payload: source port, dest port, length, checksum,
 * then the bytes. The checksum covers a fake IP header (the pseudo-header)
 * plus that datagram, so a corrupted address is caught too. A checksum
 * field of 0 means the sender did not compute one; UDP over IPv4 allows
 * that, so we accept it. Anything else must match.
 */
#include <errno.h>
#include <ipv4.h>
#include <kheap.h>
#include <random.h>
#include <string.h>
#include <timer.h>
#include <udp.h>

#define UDP_SLOTS 16
#define UDP_MAX   1472              /* 1500 - IP header - UDP header */

struct udp_slot {
    uint16_t port;                  /* 0: free */
    udp_recv_fn fn;
    void *ctx;
};

static struct udp_slot slots[UDP_SLOTS];

static struct udp_slot *find(uint16_t port)
{
    for (int i = 0; i < UDP_SLOTS; i++)
        if (slots[i].port == port)
            return &slots[i];
    return 0;
}

int udp_bind(uint16_t port, udp_recv_fn fn, void *ctx)
{
    if (!port || !fn)
        return -EINVAL;
    struct udp_slot *s = find(port);
    if (s) {
        if (s->ctx != ctx)
            return -EADDRINUSE;
        s->fn = fn;
        return 0;
    }
    s = find(0);
    if (!s)
        return -ENOMEM;
    s->port = port;
    s->fn = fn;
    s->ctx = ctx;
    return 0;
}

void udp_unbind(uint16_t port)
{
    struct udp_slot *s = port ? find(port) : 0;
    if (s)
        memset(s, 0, sizeof *s);
}

static int port_used(uint16_t p)
{
    return find(p) != 0;
}

uint16_t udp_free_port(void)
{
    return ip_ephemeral_port(port_used);
}

int udp_sendto(uint32_t dst, uint16_t sport, uint16_t dport,
               const void *data, uint32_t len)
{
    if (!sport || !dport)
        return -EINVAL;
    if (len > UDP_MAX)
        return -EMSGSIZE;
    /* kmalloc, not a static buffer: udp_sendto is called from syscalls and
       from the net thread, and one of them can be part-way through filling
       a buffer when the other runs. */
    uint8_t *pkt = kmalloc(8 + len);
    if (!pkt)
        return -ENOMEM;
    pkt[0] = (uint8_t)(sport >> 8);
    pkt[1] = (uint8_t)sport;
    pkt[2] = (uint8_t)(dport >> 8);
    pkt[3] = (uint8_t)dport;
    uint16_t ulen = (uint16_t)(8 + len);
    pkt[4] = (uint8_t)(ulen >> 8);
    pkt[5] = (uint8_t)ulen;
    pkt[6] = 0;
    pkt[7] = 0;
    if (len)
        memcpy(pkt + 8, data, len);
    uint16_t sum = ip_transport_csum(IPV4_ADDR, dst, IPPROTO_UDP, pkt, ulen);
    if (sum == 0)
        sum = 0xFFFF;               /* a computed checksum is never stored as 0 */
    pkt[6] = (uint8_t)(sum >> 8);
    pkt[7] = (uint8_t)sum;
    int rc = ip_send(dst, IPPROTO_UDP, pkt, ulen);
    kfree(pkt);
    return rc;
}

void udp_input(uint32_t src, uint32_t dst, const uint8_t *pkt, uint32_t len)
{
    (void)dst;
    if (len < 8)
        return;
    uint16_t ulen = (uint16_t)((pkt[4] << 8) | pkt[5]);
    if (ulen < 8 || ulen > len)
        return;
    uint16_t check = (uint16_t)((pkt[6] << 8) | pkt[7]);
    if (check != 0 &&
        ip_transport_csum(src, IPV4_ADDR, IPPROTO_UDP, pkt, ulen) != 0)
        return;
    uint16_t dport = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint16_t sport = (uint16_t)((pkt[0] << 8) | pkt[1]);
    struct udp_slot *s = dport ? find(dport) : 0;
    if (s)
        s->fn(s->ctx, src, sport, pkt + 8, ulen - 8);
}
