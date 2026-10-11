/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_IPV6_H
#define LINUX_IPV6_H
/* The IPv6 header (RFC 8200). We have no IPv6 stack; drivers only parse it. */
#include <linux/in6.h>
#include <linux/skbuff.h>
struct ipv6hdr {
    u8 priority:4, version:4;
    u8 flow_lbl[3];
    __be16 payload_len;
    u8 nexthdr, hop_limit;
    struct in6_addr saddr, daddr;
};
struct ipv6_opt_hdr { u8 nexthdr, hdrlen; };
#define ipv6_optlen(p) (((p)->hdrlen + 1) << 3)
static inline struct ipv6hdr *ipv6_hdr(const struct sk_buff *skb) { return (struct ipv6hdr *)skb_network_header(skb); }
#endif
