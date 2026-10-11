#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
"""
mkseeds.py — starting points for the packet fuzzers (tools/fuzz/seeds/).

A fuzzer that starts from random bytes spends a very long time discovering
that an IPv4 header begins with 0x45. Seeds skip that: each one is a packet
that already reaches something, so mutation starts from inside the parser
instead of outside it.

Shapes taken from the bugs in docs/security.md, because those are the shapes
that broke other people's stacks:

  Ripple20 (CVE-2020-11896)  a length field that disagrees with the real
                             number of bytes present
  AMNESIA:33 (CVE-2020-24338) a DNS compression pointer that points at
                             itself, so a parser that follows pointers never
                             returns
  CVE-2020-17443             a length small enough that subtracting a header
                             from it underflows
  SACK panic (CVE-2019-11477) a tiny MSS with crafted options

Run: python3 tools/fuzz/mkseeds.py
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUR_IP = bytes([10, 0, 2, 15])
PEER_IP = bytes([10, 0, 2, 2])


def write(target, name, data):
    d = os.path.join(HERE, "seeds", target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(bytes(data))


def checksum(b):
    """The same one's-complement sum net/ipv4.c uses."""
    if len(b) % 2:
        b = bytes(b) + b"\0"
    s = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ip(proto, payload, tot_len=None, ver_ihl=0x45, frag=0, dst=OUR_IP):
    body = bytes(payload)
    total = len(body) + 20 if tot_len is None else tot_len
    h = bytearray(20)
    h[0] = ver_ihl
    struct.pack_into(">H", h, 2, total & 0xFFFF)
    struct.pack_into(">H", h, 4, 0x1234)          # id
    struct.pack_into(">H", h, 6, frag)
    h[8] = 64                                     # ttl
    h[9] = proto
    h[12:16] = PEER_IP
    h[16:20] = dst
    struct.pack_into(">H", h, 10, checksum(bytes(h)))
    return bytes(h) + body


def icmp(typ, code=0, ident=1, seq=1, data=b"abcdefgh"):
    h = bytearray(8)
    h[0] = typ
    h[1] = code
    struct.pack_into(">H", h, 4, ident)
    struct.pack_into(">H", h, 6, seq)
    body = bytes(h) + data
    struct.pack_into(">H", body := bytearray(body), 2, checksum(bytes(body)))
    return bytes(body)


def udp(sport, dport, payload):
    h = bytearray(8)
    struct.pack_into(">H", h, 0, sport)
    struct.pack_into(">H", h, 2, dport)
    struct.pack_into(">H", h, 4, 8 + len(payload))
    return bytes(h) + bytes(payload)            # checksum 0: UDP may omit it


def tcp(sport=80, dport=49152, seq=0x1000, ack=0, flags=0x12, win=0xFFFF,
        opts=b"", data=b""):
    while len(opts) % 4:
        opts += b"\0"
    doff = (20 + len(opts)) // 4
    h = bytearray(20)
    struct.pack_into(">H", h, 0, sport)
    struct.pack_into(">H", h, 2, dport)
    struct.pack_into(">I", h, 4, seq)
    struct.pack_into(">I", h, 8, ack)
    h[12] = doff << 4
    h[13] = flags
    struct.pack_into(">H", h, 14, win)
    return bytes(h) + opts + bytes(data)


def name(s):
    out = b""
    for label in s.split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\0"


# --------------------------------------------------------------------- wire
# Raw bytes to on_ip, nothing repaired: the length and version checks.
write("wire", "echo", ip(1, icmp(8)))
write("wire", "short", ip(1, icmp(8))[:19])
write("wire", "ihl-zero", ip(1, icmp(8), ver_ihl=0x40))
write("wire", "ihl-max", ip(1, icmp(8), ver_ihl=0x4F))
write("wire", "v6-version", ip(1, icmp(8), ver_ihl=0x65))
# Ripple20: the header says far more bytes than arrived.
write("wire", "totlen-huge", ip(1, icmp(8), tot_len=0xFFFF))
# The mirror of it: the header claims less than its own length.
write("wire", "totlen-under", ip(1, icmp(8), tot_len=10))
write("wire", "fragment", ip(1, icmp(8), frag=0x2000))
write("wire", "frag-offset", ip(1, icmp(8), frag=0x0001))
write("wire", "not-ours", ip(1, icmp(8), dst=bytes([10, 0, 2, 99])))
write("wire", "broadcast", ip(1, icmp(8), dst=bytes([255, 255, 255, 255])))
write("wire", "multicast", ip(1, icmp(8), dst=bytes([224, 0, 0, 1])))

# ----------------------------------------------------------------------- ip
# The header is repaired by the harness, so these are about the body.
write("ip", "echo", ip(1, icmp(8)))
write("ip", "echo-empty", ip(1, icmp(8, data=b"")))
write("ip", "echo-big", ip(1, icmp(8, data=b"A" * 1400)))
write("ip", "echo-reply", ip(1, icmp(0)))
write("ip", "icmp-trunc", ip(1, b"\x08\x00"))
write("ip", "icmp-unreach", ip(1, icmp(3, code=4, data=ip(6, tcp()))))
write("ip", "udp-dns", ip(17, udp(53, 49152, b"\0" * 12)))
write("ip", "udp-len-lie", ip(17, bytes([0, 53, 0xC0, 0, 0xFF, 0xFF, 0, 0]) + b"x" * 8))
# CVE-2020-17443 shape: a length that underflows when a header is subtracted.
write("ip", "udp-len-tiny", ip(17, bytes([0, 53, 0xC0, 0, 0, 1, 0, 0])))
write("ip", "udp-port-zero", ip(17, udp(53, 0, b"abcd")))
write("ip", "tcp-synack", ip(6, tcp()))
write("ip", "tcp-rst", ip(6, tcp(flags=0x04)))
write("ip", "tcp-doff-huge", ip(6, bytearray(tcp())[:12] + bytes([0xF0]) + bytes(tcp()[13:])))
write("ip", "tcp-doff-zero", ip(6, bytearray(tcp())[:12] + bytes([0x00]) + bytes(tcp()[13:])))
# SACK panic shape: a tiny MSS, plus SACK-permitted.
write("ip", "tcp-mss-48", ip(6, tcp(opts=bytes([2, 4, 0, 48, 4, 2]))))
write("ip", "tcp-mss-zero", ip(6, tcp(opts=bytes([2, 4, 0, 0]))))
# An option whose length byte says zero: the classic parser infinite loop.
write("ip", "tcp-opt-len0", ip(6, tcp(opts=bytes([8, 0, 0, 0]))))
write("ip", "tcp-opt-len-huge", ip(6, tcp(opts=bytes([8, 0xFF, 0, 0]))))
write("ip", "tcp-win-zero", ip(6, tcp(flags=0x10, win=0)))
write("ip", "proto-unknown", ip(254, b"\xff" * 40))

# ---------------------------------------------------------------------- arp
def arp(oper=1, sha=b"\x52\x55\x0a\x00\x02\x02", spa=PEER_IP,
        tha=b"\0" * 6, tpa=OUR_IP, htype=1, ptype=0x0800, hlen=6, plen=4):
    h = bytearray(28)
    struct.pack_into(">H", h, 0, htype)
    struct.pack_into(">H", h, 2, ptype)
    h[4] = hlen
    h[5] = plen
    struct.pack_into(">H", h, 6, oper)
    h[8:14] = sha
    h[14:18] = spa
    h[18:24] = tha
    h[24:28] = tpa
    return bytes(h)


write("arp", "request", arp(1))
write("arp", "reply", arp(2))
write("arp", "short", arp(1)[:20])
write("arp", "oper-huge", arp(0xFFFF))
write("arp", "hlen-big", arp(1, hlen=0xFF))
write("arp", "plen-big", arp(1, plen=0xFF))
write("arp", "sha-broadcast", arp(2, sha=b"\xff" * 6))
write("arp", "sha-zero", arp(2, sha=b"\0" * 6))
write("arp", "sha-multicast", arp(2, sha=b"\x01\x00\x5e\x00\x00\x01"))
write("arp", "spa-ours", arp(2, spa=OUR_IP))          # claiming to be us
write("arp", "spa-broadcast", arp(2, spa=b"\xff" * 6))
write("arp", "tpa-other", arp(1, tpa=bytes([10, 0, 2, 99])))

# ---------------------------------------------------------------------- dns
# Input is [control][12-byte header][answer section]; the harness splices the
# real question in between, because on_dns byte-compares it.
QLEN = len(name("example.com")) + 4           # question bytes the harness adds
ANS_AT = 12 + QLEN                            # where the answer section lands


def dns_hdr(an=1, flags=0x8180, qd=1, ns=0, ar=0):
    h = bytearray(12)
    struct.pack_into(">H", h, 0, 0)            # id, patched by the harness
    struct.pack_into(">H", h, 2, flags)
    struct.pack_into(">H", h, 4, qd)
    struct.pack_into(">H", h, 6, an)
    struct.pack_into(">H", h, 8, ns)
    struct.pack_into(">H", h, 10, ar)
    return bytes(h)


def rr(nm, typ=1, cls=1, ttl=60, rdata=bytes([104, 20, 23, 154])):
    return nm + struct.pack(">HHIH", typ, cls, ttl, len(rdata)) + rdata


PTR_Q = bytes([0xC0, 0x0C])                    # points at the question
A_REC = rr(PTR_Q)

write("dns", "answer", bytes([1]) + dns_hdr() + A_REC)
write("dns", "answer-literal", bytes([1]) + dns_hdr() + rr(name("example.com")))
write("dns", "no-answer", bytes([1]) + dns_hdr(an=0))
write("dns", "nxdomain", bytes([1]) + dns_hdr(an=0, flags=0x8183))
write("dns", "not-a-response", bytes([1]) + dns_hdr(flags=0x0100))
write("dns", "wrong-id", bytes([0]) + dns_hdr() + A_REC)
write("dns", "qd-zero", bytes([1]) + dns_hdr(qd=0) + A_REC)
write("dns", "qd-many", bytes([1]) + dns_hdr(qd=9) + A_REC)
write("dns", "an-huge", bytes([1]) + dns_hdr(an=0xFFFF) + A_REC)
write("dns", "an-lies", bytes([1]) + dns_hdr(an=16) + A_REC)   # says 16, sends 1
# AMNESIA:33 / CVE-2020-24338: a pointer to itself. A parser that follows
# compression pointers never comes back.
write("dns", "ptr-self", bytes([1]) + dns_hdr() + rr(struct.pack(">H", 0xC000 | ANS_AT)))
write("dns", "ptr-forward", bytes([1]) + dns_hdr() + rr(struct.pack(">H", 0xC000 | (ANS_AT + 2))))
write("dns", "ptr-past-end", bytes([1]) + dns_hdr() + rr(bytes([0xC1, 0xFF])))
write("dns", "ptr-chain", bytes([1]) + dns_hdr() +
      struct.pack(">H", 0xC000 | (ANS_AT + 2)) + struct.pack(">H", 0xC000 | ANS_AT))
# A label that runs off the end of the message.
write("dns", "label-overrun", bytes([1]) + dns_hdr() + bytes([0x3F]) + b"A" * 8)
write("dns", "label-max", bytes([1]) + dns_hdr() + bytes([0x3F]) + b"A" * 63 + b"\0" + A_REC[2:])
# Reserved label type 0b10xxxxxx.
write("dns", "label-reserved", bytes([1]) + dns_hdr() + bytes([0x80, 0x01]) + A_REC[2:])
# rdlength that disagrees with what follows, both ways.
write("dns", "rdlen-huge", bytes([1]) + dns_hdr() + PTR_Q + struct.pack(">HHIH", 1, 1, 60, 0xFFFF) + b"\x01\x02\x03\x04")
write("dns", "rdlen-zero", bytes([1]) + dns_hdr() + PTR_Q + struct.pack(">HHIH", 1, 1, 60, 0))
write("dns", "rdlen-wrong", bytes([1]) + dns_hdr() + PTR_Q + struct.pack(">HHIH", 1, 1, 60, 16) + b"Z" * 16)
write("dns", "cname-then-a", bytes([1]) + dns_hdr(an=2) +
      rr(PTR_Q, typ=5, rdata=name("www.example.com")) + rr(name("www.example.com")))
# An A record about a name nobody asked about: the missing bailiwick check.
write("dns", "foreign-a", bytes([1]) + dns_hdr() + rr(name("evil.example")))
write("dns", "truncated", bytes([1]) + dns_hdr() + A_REC[:6])
write("dns", "header-only", bytes([1]) + dns_hdr(an=1))

# ---------------------------------------------------------------------- tcp
# Input is a sequence of [control][length][segment]; control bit 0 repairs
# the checksum, bit 1 supplies a plausible sequence and ack, bit 2 advances
# the clock and runs the timers.
def step(seg, ctl=3):
    return bytes([ctl, len(seg)]) + bytes(seg)


write("tcp", "synack", step(tcp(flags=0x12)))
write("tcp", "synack-then-data", step(tcp(flags=0x12)) + step(tcp(flags=0x18, data=b"hello")))
write("tcp", "handshake-fin", step(tcp(flags=0x12)) + step(tcp(flags=0x11)))
write("tcp", "rst", step(tcp(flags=0x04)))
write("tcp", "rst-blind", step(tcp(flags=0x04), ctl=1))      # wrong sequence
write("tcp", "syn-again", step(tcp(flags=0x12)) + step(tcp(flags=0x02)))
write("tcp", "win-zero", step(tcp(flags=0x12, win=0)) + step(tcp(flags=0x10, win=0), ctl=7))
write("tcp", "ack-future", step(tcp(flags=0x10, ack=0x7FFFFFFF), ctl=1))
write("tcp", "ack-past", step(tcp(flags=0x12)) + step(tcp(flags=0x10, ack=0), ctl=1))
write("tcp", "mss-tiny", step(tcp(flags=0x12, opts=bytes([2, 4, 0, 1]))))
write("tcp", "mss-huge", step(tcp(flags=0x12, opts=bytes([2, 4, 0xFF, 0xFF]))))
write("tcp", "opt-len-zero", step(tcp(flags=0x12, opts=bytes([8, 0, 0, 0]))))
write("tcp", "data-no-conn", step(tcp(flags=0x18, data=b"x" * 64), ctl=1))
write("tcp", "timers", step(tcp(flags=0x12), ctl=7) + step(tcp(flags=0x10), ctl=7))
write("tcp", "many-small", b"".join(step(tcp(flags=0x10, seq=0x1000 + i)) for i in range(8)))
write("tcp", "flags-all", step(tcp(flags=0xFF)))
write("tcp", "flags-none", step(tcp(flags=0x00)))

total = 0
for root, _, files in os.walk(os.path.join(HERE, "seeds")):
    total += len(files)
print("wrote %d seeds to %s" % (total, os.path.join(HERE, "seeds")))
