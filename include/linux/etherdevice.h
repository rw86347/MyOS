#ifndef LINUX_ETHERDEVICE_H
#define LINUX_ETHERDEVICE_H

/* Ethernet (MAC) addresses: 6 bytes; bit 0 of the first byte says
   multicast, all ones is broadcast. */
#include <linux/if_ether.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/types.h>

static inline bool is_zero_ether_addr(const u8 *a) { return !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]); }
static inline bool is_multicast_ether_addr(const u8 *a) { return a[0] & 1; }
static inline bool is_local_ether_addr(const u8 *a) { return a[0] & 2; }
static inline bool is_broadcast_ether_addr(const u8 *a) { return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff; }
static inline bool is_unicast_ether_addr(const u8 *a) { return !is_multicast_ether_addr(a); }
static inline bool is_valid_ether_addr(const u8 *a) { return !is_multicast_ether_addr(a) && !is_zero_ether_addr(a); }
static inline bool ether_addr_equal(const u8 *a, const u8 *b) { return !memcmp(a, b, ETH_ALEN); }
#define ether_addr_equal_unaligned(a, b) ether_addr_equal(a, b)
static inline bool ether_addr_equal_masked(const u8 *a, const u8 *b, const u8 *m)
{ for (int i = 0; i < ETH_ALEN; i++) if ((a[i] ^ b[i]) & m[i]) return false; return true; }
static inline void ether_addr_copy(u8 *d, const u8 *s) { memcpy(d, s, ETH_ALEN); }
static inline void eth_zero_addr(u8 *a) { memset(a, 0, ETH_ALEN); }
static inline void eth_broadcast_addr(u8 *a) { memset(a, 0xff, ETH_ALEN); }
static inline void eth_random_addr(u8 *a) { get_random_bytes(a, ETH_ALEN); a[0] &= 0xfe; a[0] |= 0x02; }
static inline u64 ether_addr_to_u64(const u8 *a) { u64 v = 0; for (int i = 0; i < 6; i++) v = v << 8 | a[i]; return v; }
static inline void u64_to_ether_addr(u64 v, u8 *a) { for (int i = 5; i >= 0; i--, v >>= 8) a[i] = (u8)v; }
struct net_device;
static inline void eth_hw_addr_set(struct net_device *dev, const u8 *addr);
static inline void eth_hw_addr_random(struct net_device *dev);
#define dev_addr_set(dev, a)  eth_hw_addr_set(dev, a)
int mac_pton(const char *s, u8 *mac);

#include <netdevice.h>
static inline void eth_hw_addr_set(struct net_device *dev, const u8 *addr) { ether_addr_copy(dev->dev_addr, addr); }
static inline void eth_hw_addr_random(struct net_device *dev) { eth_random_addr(dev->dev_addr); }
struct ethhdr { u8 h_dest[ETH_ALEN]; u8 h_source[ETH_ALEN]; __be16 h_proto; } __attribute__((packed));
static inline struct ethhdr *eth_hdr(const struct sk_buff *skb) { return (struct ethhdr *)(skb->head + skb->mac_header); }

#endif
