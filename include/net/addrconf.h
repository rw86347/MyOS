#ifndef NET_ADDRCONF_H
#define NET_ADDRCONF_H
/* IPv6 address configuration: none here (no IPv6 stack), so a driver's
   "my IPv6 addresses" list is always empty. */
#include <net/ipv6.h>
struct inet6_dev;
struct inet6_ifaddr;
static inline struct inet6_dev *__in6_dev_get(const struct net_device *d) { (void)d; return NULL; }
static inline struct inet6_dev *in6_dev_get(const struct net_device *d) { (void)d; return NULL; }
static inline void in6_dev_put(struct inet6_dev *i) { (void)i; }
#endif
