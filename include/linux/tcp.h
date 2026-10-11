/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_TCP_H
#define LINUX_TCP_H
/* The TCP header (RFC 9293), x86 bitfield order. */
#include <linux/skbuff.h>
#include <linux/types.h>
struct tcphdr {
    __be16 source, dest;
    __be32 seq, ack_seq;
    u16 res1:4, doff:4, fin:1, syn:1, rst:1, psh:1, ack:1, urg:1, ece:1, cwr:1;
    __be16 window;
    __sum16 check;
    __be16 urg_ptr;
};
static inline struct tcphdr *tcp_hdr(const struct sk_buff *skb) { return (struct tcphdr *)skb_transport_header(skb); }
static inline unsigned int tcp_hdrlen(const struct sk_buff *skb) { return tcp_hdr(skb)->doff * 4u; }
#endif
