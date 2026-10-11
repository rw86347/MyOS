/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_IP_H
#define LINUX_IP_H
/* The IPv4 header (RFC 791), little-endian bitfield order as on x86. */
#include <linux/skbuff.h>
#include <linux/types.h>
#define IPPROTO_IP     0
#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define IPPROTO_IPV6   41
#define IPPROTO_ICMPV6 58
struct iphdr {
    u8 ihl:4, version:4;
    u8 tos;
    __be16 tot_len, id, frag_off;
    u8 ttl, protocol;
    __sum16 check;
    __be32 saddr, daddr;
};
static inline struct iphdr *ip_hdr(const struct sk_buff *skb) { return (struct iphdr *)skb_network_header(skb); }
static inline unsigned int ip_hdrlen(const struct sk_buff *skb) { return ip_hdr(skb)->ihl * 4u; }
#endif
