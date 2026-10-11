/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef GENETLINK_H
#define GENETLINK_H

/*
 * The kernel side of netlink, for code outside net/netlink.c: building a
 * message, answering a request, reading its attributes, and sending an
 * event to the programs that joined a multicast group. Generic netlink
 * families live behind it (nl80211: net/wireless/nl80211.c).
 *
 * A message is a fixed header followed by attributes { u16 len; u16
 * type; value }, each padded to 4 bytes. It is built in a buffer the
 * caller owns (often on the stack); one that would overflow it stops
 * growing and is marked `over`, and is then not sent.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct nlmsghdr { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nlattr { uint16_t len, type; };
struct genlmsghdr { uint8_t cmd, version; uint16_t reserved; };

#define NLM_F_REQUEST   0x001
#define NLM_F_MULTI     0x002
#define NLM_F_ACK       0x004
#define NLM_F_DUMP      0x300
#define NLA_F_NESTED    0x8000
#define NLA_TYPE_MASK   0x3fff          /* the top 2 bits of a type are flags */
#define NLA_ALIGN(n)    (((n) + 3u) & ~3u)

struct nl_msg {
    uint8_t *b;
    uint32_t len, cap;
    int over;
};
#define NL_MSG(name, buf) struct nl_msg name = { (buf), 0, sizeof(buf), 0 }

void *nl_put(struct nl_msg *m, const void *p, uint32_t n);     /* raw bytes, padded */
void nla_put(struct nl_msg *m, uint16_t type, const void *p, uint32_t n);
static inline void nla_put_flag(struct nl_msg *m, uint16_t t) { nla_put(m, t, NULL, 0); }
static inline void nla_put_u8(struct nl_msg *m, uint16_t t, uint8_t v) { nla_put(m, t, &v, 1); }
static inline void nla_put_u16(struct nl_msg *m, uint16_t t, uint16_t v) { nla_put(m, t, &v, 2); }
static inline void nla_put_u32(struct nl_msg *m, uint16_t t, uint32_t v) { nla_put(m, t, &v, 4); }
static inline void nla_put_s32(struct nl_msg *m, uint16_t t, int32_t v) { nla_put(m, t, &v, 4); }
static inline void nla_put_u64(struct nl_msg *m, uint16_t t, uint64_t v) { nla_put(m, t, &v, 8); }
static inline void nla_put_string(struct nl_msg *m, uint16_t t, const char *s)
{
    nla_put(m, t, s, (uint32_t)strlen(s) + 1);
}
/* A nested attribute: start it, put its members, end it (sets its length). */
uint32_t nla_nest_start(struct nl_msg *m, uint16_t type);
void nla_nest_end(struct nl_msg *m, uint32_t at);

/* Reading: tb[type] = the attribute of that type in p (the last one, as
   Linux), for types 1..maxtype. Returns -EINVAL for a malformed list. */
int nla_parse(const struct nlattr **tb, int maxtype, const void *p, uint32_t len);
static inline const void *nla_data(const struct nlattr *a) { return a + 1; }
static inline uint32_t nla_len(const struct nlattr *a) { return a->len - (uint32_t)sizeof *a; }
static inline uint16_t nla_type(const struct nlattr *a) { return a->type & NLA_TYPE_MASK; }
static inline uint8_t nla_get_u8(const struct nlattr *a) { return *(const uint8_t *)nla_data(a); }
static inline uint16_t nla_get_u16(const struct nlattr *a) { uint16_t v; memcpy(&v, nla_data(a), 2); return v; }
static inline uint32_t nla_get_u32(const struct nlattr *a) { uint32_t v; memcpy(&v, nla_data(a), 4); return v; }
static inline uint64_t nla_get_u64(const struct nlattr *a) { uint64_t v; memcpy(&v, nla_data(a), 8); return v; }
/* Each attribute nested inside a (bounds-checked). */
#define nla_for_each_nested(pos, a, rem)                                              \
    for ((pos) = (const struct nlattr *)nla_data(a), (rem) = (int)nla_len(a);        \
         (rem) >= (int)sizeof(struct nlattr) && (pos)->len >= sizeof(struct nlattr) && \
         (int)(pos)->len <= (rem);                                                     \
         (rem) -= (int)NLA_ALIGN((pos)->len),                                          \
         (pos) = (const struct nlattr *)((const uint8_t *)(pos) + NLA_ALIGN((pos)->len)))

/* The answers to one request. */
struct nl_reply;
void nl_reply(struct nl_reply *r, uint16_t type, uint16_t flags, const struct nl_msg *m);
void nl_reply_done(struct nl_reply *r);                 /* the end of a dump */
int nl_reply_is_dump(const struct nl_reply *r);
uint32_t nl_reply_seq(const struct nl_reply *r);
uint32_t nl_reply_portid(const struct nl_reply *r);   /* the asking socket's address */

/* A generic netlink family. Its multicast groups get the ids group_base,
   group_base + 1, ... (each one bit of a socket's 64-bit group mask).
   request answers one message whose payload (from the genlmsghdr on) is
   p, plen bytes; it returns 0 or -errno (then an error is sent). */
struct genl_family_k {
    const char *name;
    uint16_t id;
    uint32_t version, hdrsize, maxattr;
    const char *const *groups;          /* NULL-terminated */
    uint32_t group_base;
    int (*request)(struct nl_reply *r, const uint8_t *p, uint32_t plen);
};

/* Send m (a genl payload) as an event to every socket in the group. */
void genl_multicast(const struct genl_family_k *f, uint32_t group, const struct nl_msg *m);

#endif
