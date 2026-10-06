#ifndef NET_IPV6_H
#define NET_IPV6_H
#include <linux/ipv6.h>
#include <net/checksum.h>
#define NEXTHDR_HOP      0
#define NEXTHDR_TCP      6
#define NEXTHDR_UDP      17
#define NEXTHDR_ROUTING  43
#define NEXTHDR_FRAGMENT 44
#define NEXTHDR_NONE     59
#define NEXTHDR_DEST     60
static inline bool ipv6_ext_hdr(u8 h)
{ return h == NEXTHDR_HOP || h == NEXTHDR_ROUTING || h == NEXTHDR_FRAGMENT || h == 51 || h == NEXTHDR_NONE || h == NEXTHDR_DEST; }
static inline int ipv6_addr_cmp(const struct in6_addr *a, const struct in6_addr *b) { return memcmp(a, b, sizeof *a); }
static inline bool ipv6_addr_any(const struct in6_addr *a) { return !(a->s6_addr32[0] | a->s6_addr32[1] | a->s6_addr32[2] | a->s6_addr32[3]); }
__sum16 csum_ipv6_magic(const struct in6_addr *s, const struct in6_addr *d, u32 len, u8 proto, __wsum csum);
#endif
