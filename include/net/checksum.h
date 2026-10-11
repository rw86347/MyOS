/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef NET_CHECKSUM_H
#define NET_CHECKSUM_H
/* The Internet checksum (RFC 1071): ones' complement sums. linux_lib.c. */
#include <linux/types.h>
__wsum csum_partial(const void *buf, int len, __wsum sum);
__sum16 csum_fold(__wsum sum);
static inline __wsum csum_unfold(__sum16 n) { return (__wsum)n; }
static inline __wsum csum_add(__wsum a, __wsum b) { u32 r = a + b; return r + (r < b); }
static inline __wsum csum_sub(__wsum a, __wsum b) { return csum_add(a, ~b); }
__sum16 ip_fast_csum(const void *iph, unsigned int ihl);
__wsum csum_tcpudp_nofold(__be32 saddr, __be32 daddr, u32 len, u8 proto, __wsum sum);
static inline __sum16 csum_tcpudp_magic(__be32 s, __be32 d, u32 len, u8 proto, __wsum sum)
{ return csum_fold(csum_tcpudp_nofold(s, d, len, proto, sum)); }
#endif
