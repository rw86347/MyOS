#ifndef IPV4_H
#define IPV4_H

#include <stdint.h>

/*
 * A small IPv4 stack: ARP, ICMP, UDP, TCP, DNS. One address (QEMU's
 * 10.0.2.15 until DHCP exists). No IP options, no fragments. A packet
 * whose lengths do not fit is dropped.
 */

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/* QEMU user-net, until DHCP exists. Host order. */
#define IPV4_ADDR  0x0A00020Fu      /* 10.0.2.15 */
#define IPV4_GW    0x0A000202u      /* 10.0.2.2 */
#define IPV4_DNS   0x0A000203u      /* 10.0.2.3 */

void ipv4_init(void);

/* ICMP echo to dst (host order). 0 if a reply arrives within a second. */
int net_ping(uint32_t dst);

/* Send one IP payload. Resolves ARP if the neighbor is not cached.
   0, or -errno. Not for an interrupt handler: ARP may wait. */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len);

/* Internet checksum. Add walks bytes in wire order; finish returns the
   value that goes in the checksum field (host integer, big-endian on the wire). */
uint32_t ip_csum_add(uint32_t sum, const void *data, uint32_t len);
uint16_t ip_csum_finish(uint32_t sum);

/* UDP/TCP checksum over the pseudo-header plus the segment. 0 means the
   segment (checksum field included) is valid. */
uint16_t ip_transport_csum(uint32_t src, uint32_t dst, uint8_t proto,
                           const void *seg, uint32_t len);

#endif
