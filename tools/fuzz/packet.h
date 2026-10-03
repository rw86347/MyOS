#ifndef FUZZ_PACKET_H
#define FUZZ_PACKET_H

/*
 * Building packets and handing them to the stack, shared by the fuzz targets
 * (targets.c) and the adversarial tests (attacks.c). Both need to put a
 * frame on the wire and see what comes back; only what they do with the
 * answer differs.
 */

#include <ipv4.h>
#include <skbuff.h>
#include <stdint.h>
#include <string.h>

/*
 * Wire offsets rather than structs copied out of net/ipv4.c: these are
 * formats fixed by RFC 791/792/768/793, and a private copy of a kernel
 * struct would be one more thing that can quietly disagree with it.
 */
#define IP_VER_IHL  0
#define IP_TOT_LEN  2
#define IP_ID       4
#define IP_FRAG     6
#define IP_TTL      8
#define IP_PROTO    9
#define IP_CHECK   10
#define IP_SADDR   12
#define IP_DADDR   16
#define IP_HLEN    20

#define ETH_DST     0
#define ETH_SRC     6
#define ETH_TYPE   12

#define ICMP_TYPE   0
#define ICMP_CODE   1
#define ICMP_CHECK  2
#define ICMP_ID     4
#define ICMP_SEQ    6

#define ARP_OPER    6
#define ARP_SHA     8
#define ARP_SPA    14
#define ARP_THA    18
#define ARP_TPA    24
#define ARP_LEN    28

#define OUR_IP    ipv4_addr         /* 10.0.2.15 */
#define PEER_IP   0x0A000202u       /* 10.0.2.2, on our subnet */
#define PEER_PORT 80

extern const uint8_t peer_mac[6];
extern const uint8_t evil_mac[6];

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* The kernel's own checksum, reused so the harness cannot disagree with it. */
uint16_t pk_csum(const void *d, uint32_t n);

/* Hand a frame to the protocol handler for `type`, built to look like what
   eth_type_trans leaves behind: Ethernet header already pulled, protocol
   set, sender MAC in eth_src. The handler frees it. */
void pk_feed(uint16_t type, const uint8_t *b, uint32_t n, const uint8_t mac[6]);

/* An IP header in front of `body`, addressed to us from PEER_IP, with the
   checksum filled in. Returns the total length written to `out`. */
uint32_t pk_ip(uint8_t *out, uint32_t cap, uint8_t proto,
               const uint8_t *body, uint32_t n);
void     pk_ip_set_dst(uint8_t *pkt, uint32_t dst);   /* and re-checksum */
void     pk_ip_set_src(uint8_t *pkt, uint32_t src);

void pk_arp(uint8_t out[ARP_LEN], uint16_t oper, const uint8_t sha[6],
            uint32_t spa, uint32_t tpa);

/* Where the pump sends IP frames the stack transmitted. NULL to drop them. */
void pk_set_ip_sink(void (*fn)(const uint8_t *ip, uint32_t len));

/* Drain everything the stack sent and answer any ARP request, which is what
   releases frames parked waiting on one. Loops until quiet. */
void pk_pump(void);

/* Resolve the neighbours a test or target talks to. Idempotent. */
void pk_prime_arp(void);

#endif
