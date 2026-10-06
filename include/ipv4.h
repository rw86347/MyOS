#ifndef IPV4_H
#define IPV4_H

#include <stdint.h>

/*
 * A small IPv4 stack: ARP, ICMP, UDP, TCP, DNS. One address, one subnet,
 * one gateway. No IP options, no fragments. A packet whose lengths do not
 * fit is dropped.
 */

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/*
 * This machine's address, learned by DHCP at boot (net/dhcp.c). Host order.
 * Zero until a lease arrives, which is also how the receive path knows it
 * has no address yet.
 *
 * Plain words rather than a struct behind an accessor because every send
 * reads one: a load, not a call. They belong to the one device IPv4 runs on
 * at a time (ipv4_dev(): eth0, or wlan0 once Wi-Fi connects — a laptop's
 * situation); switching devices swaps them, and connections made on the
 * old one stop working, as when a laptop changes networks.
 */
extern uint32_t ipv4_addr, ipv4_mask, ipv4_gw, ipv4_dns;

/* The device those belong to, or NULL if there is none. */
struct net_device;
struct net_device *ipv4_dev(void);

/* A link became usable (Wi-Fi: associated and authorized): IPv4 moves to it,
   with the lease it had, or asks DHCP for one in a thread of its own. When
   the active link goes, IPv4 moves to another one that is up, if any. */
void ipv4_link_up(struct net_device *dev);
void ipv4_link_down(struct net_device *dev);

/* What to use if no DHCP server answers. These are QEMU user-net's own
   numbers, which is also what its built-in DHCP server hands out, so a
   fallback and a lease look the same there. */
#define IPV4_ADDR_STATIC  0x0A00020Fu   /* 10.0.2.15 */
#define IPV4_MASK_STATIC  0xFFFFFF00u
#define IPV4_GW_STATIC    0x0A000202u   /* 10.0.2.2 */
#define IPV4_DNS_STATIC   0x0A000203u   /* 10.0.2.3 */

void ipv4_init(void);

/*
 * DHCP's one exception to the rules in here: a UDP datagram sent from 0.0.0.0
 * to 255.255.255.255, which every other path refuses. Broadcast is how a
 * machine with no address asks for one, and it is also how a machine becomes
 * an amplifier, so the exception is one function with one caller.
 */
int ip_send_bootp(const void *udp, uint32_t len);

/* ICMP echo to dst (host order). 0 if a reply arrives within a second. */
int net_ping(uint32_t dst);

/* Send one IP payload. Resolves ARP if the neighbor is not cached.
   0, or -errno. Not for an interrupt handler: ARP may wait. */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len);

/*
 * An unguessable ephemeral port, 49152..65535 (RFC 6056). `used` returns
 * nonzero for a port already taken. 0 if every port is in use. Both the TCP
 * and UDP port tables go through this, so there is one place to get it right.
 */
uint16_t ip_ephemeral_port(int (*used)(uint16_t));

/*
 * May we send a packet we are only sending because one arrived from `peer`?
 * An echo reply, an ARP reply, a reset for a connection that does not exist:
 * one packet in must not mean one packet out without limit, or we are an
 * amplifier. Per-peer and jittered on purpose — see net/ipv4.c.
 */
int ip_rate_ok(uint32_t peer);

/* Internet checksum. Add walks bytes in wire order; finish returns the
   value that goes in the checksum field (host integer, big-endian on the wire). */
uint32_t ip_csum_add(uint32_t sum, const void *data, uint32_t len);
uint16_t ip_csum_finish(uint32_t sum);

/* UDP/TCP checksum over the pseudo-header plus the segment. 0 means the
   segment (checksum field included) is valid. */
uint16_t ip_transport_csum(uint32_t src, uint32_t dst, uint8_t proto,
                           const void *seg, uint32_t len);

#endif
