#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

/*
 * A DHCP client: enough of RFC 2131 to be given an address, a netmask, a
 * gateway and a nameserver, and nothing else.
 *
 * It runs once, from ipv4_init, before any socket exists, and it blocks:
 * almost everything above it needs an address, so there is nothing useful to
 * do in the meantime, and a boot that carries on without one would have to
 * make every send fail for a while instead. It is bounded (see DHCP_TRIES in
 * net/dhcp.c) so a network with no server delays the boot by a known amount
 * and no more, and then the static fallback in include/ipv4.h is used --
 * which is what makes `make run` under QEMU behave the same either way.
 *
 * No lease renewal yet: a lease is taken and kept. See docs/gaps.md.
 */

/* Ask for an address. Writes ipv4_addr/mask/gw/dns and returns 0, or falls
   back to the static numbers and returns -errno. */
int dhcp_configure(void);

/* One DHCP exchange on dev (the device IPv4 runs on): the lease, or -errno. */
struct net_device;
struct dhcp_lease { uint32_t addr, mask, gw, dns; };
int dhcp_lease(struct net_device *dev, struct dhcp_lease *out);

/* Whether a transaction is open, which is the only time the receive path
   accepts a broadcast (net/ipv4.c says why). False before and after. */
int dhcp_open(void);

/* Parse one BOOTP reply: the body of a UDP datagram that arrived on port 68.
   Separate from the rest so the fuzz target can reach it (tools/fuzz). */
void dhcp_input(uint32_t src, uint16_t sport, const uint8_t *pkt, uint32_t len);

#endif
