# MyOS Network Security

The goal: a stack that an attacker with a wire to the machine cannot get code execution on,
and cannot knock off the network. Small helps here — 3,400 lines of network code can be read
end to end and fuzzed exhaustively in a way that Linux's 1.5 million cannot. That is the one
real advantage we have, and it is only an advantage if we spend it.

This file holds the threat model, the rules new network code must follow, and the plan.
Shortcuts we know about live in `docs/gaps.md`; measurements in `docs/performance.md`.

## Threat model

Who we defend against, in order of how much it matters:

1. **Off-path attacker on the Internet.** Cannot see our traffic. Can send packets with any
   source address. Breaks in by *guessing* — sequence numbers, ports, DNS transaction IDs —
   or by sending malformed packets to a parser. This is the attacker who gets remote code
   execution, and the one all the CVEs below belong to.
2. **Attacker on the same Ethernet segment** (coffee-shop Wi-Fi, a hostile LAN). Can do
   everything above, plus see our traffic, plus answer ARP and DHCP faster than the real
   server. Cannot be stopped by a network stack alone: the answer is to not trust the
   network, which means TLS in user space. What we *can* do is not make it easy, and not
   let a hostile neighbour take the machine down.
3. **A malicious local program** (once step 11 has users). Must not reach the wire raw, read
   another process's packets, or exhaust the kernel's fixed tables.

Out of scope: physical access, and an attacker who can both see and modify our traffic.
Neither is a network-stack problem. Do not pretend otherwise in code comments.

## How network stacks actually get broken

Researched from the public record. Nearly every remote code execution in a TCP/IP stack in
the last decade falls into one of four buckets, and the first two cover most of them.

### 1. A length from the packet is trusted to size or bound a copy

The dominant bug. The pattern is always the same: one piece of code computes how big a
buffer should be, another fills it, and an attacker makes the two disagree.

- **Ripple20 / CVE-2020-11896** (Treck, CVSS 10, remote code execution). IP reassembly
  *linked* fragments instead of copying them. An inner IP-in-IP packet trimmed the
  "total chain length" field while the fragment list stayed long. UDP later allocated a
  buffer from the trimmed length and then `tfCopyPacket` walked the whole fragment list
  with no bounds check. Heap overflow. Two sources of truth for one length.
- **AMNESIA:33** (uIP, picoTCP, FNET, Nut/Net — 33 bugs, 4 of them RCE). Header length
  fields not checked against the bytes actually present: TCP/UDP length fields used in the
  checksum loop causing out-of-bounds reads, IPv6 extension-header lengths, IPv4 total
  length and fragment offsets used in reassembly.
- **CVE-2020-24338 / CVE-2020-24336 / CVE-2020-25111** — all three are DNS *name*
  parsing. Domain-name decompression writing into a fixed buffer, and names assumed to be
  null-terminated when the packet never said so.

The lesson is sharper than "check your lengths": **the RCEs cluster in reassembly and
decompression** — the two places where a stack writes into a buffer whose size came from a
different computation than the data filling it.

### 2. Integer arithmetic on attacker-controlled numbers

- **CVE-2020-17443** (picoTCP ICMPv6). `echo->transport_len - 8` where `transport_len`
  could be under 8. The unsigned result is about four billion, and it went straight to
  `memcpy`. One missing `>=` check.
- **CVE-2019-11477, "SACK Panic"** (Linux, remotely triggerable kernel panic). A peer
  advertises the smallest legal MSS (48 bytes), then sends crafted selective
  acknowledgements. Linux coalesces retransmit buffers to match the SACK blocks, and the
  **16-bit** `tcp_gso_segs` segment counter overflows, hitting a `BUG_ON`. Note what this
  one is: not a missing bounds check, but a counter too narrow for what the protocol allows.
- **uIP TCP urgent pointer** (AMNESIA:33). The urgent-data offset was subtracted from the
  packet length with no sanity check, so a large pointer and a small length underflowed.

### 3. Loops and state an attacker can drive

- **uIP MSS option parsing**: a TCP option with a bogus length field advances the loop
  index by zero. Infinite loop in an interrupt-adjacent path.
- **DNS name decompression**: a compression pointer that points at itself, or two that
  point at each other. Classic unbounded recursion.
- Unbounded reassembly queues and half-open connection tables: resource exhaustion that
  needs no bug at all, just patience.

### 4. Predictable secrets

No memory corruption, and arguably worse, because it is silent.

- **TCP initial sequence numbers** (RFC 6528). If an attacker can predict the ISN a host
  will choose, it can complete a handshake as a machine it cannot even see, or reset and
  inject into a connection it cannot observe. The historical BSD bug was incrementing a
  counter by a constant per connection.
- **Ephemeral port numbers** (RFC 6056). The four-tuple is the other half of the secret.
  Sequential ports hand it over.
- **DNS transaction IDs** — the Kaminsky attack (2008). 16 bits of ID is not enough; the
  fix was to randomize the *source port* too, taking an off-path attacker from 16 bits of
  guessing to about 32.
- **SAD DNS** (2020) is worth knowing because of its shape: source-port randomization was
  correct, and the attackers recovered the port anyway through a **side channel** — the
  global ICMP rate limiter. Probing which ports were closed leaked the open one. A shared,
  global, *exactly* rate-limited counter is an oracle. The same mistake in Linux's TCP is
  CVE-2016-5696: a global challenge-ACK rate limit let an off-path attacker learn whether
  two hosts were talking, and then their sequence numbers.

The structural lesson from the last two: **any global counter whose behaviour an attacker
can observe is a side channel.** Rate limits must be per-connection, or randomized, or both.

## Rules for network code in MyOS

Binding for everything under `net/` and for any driver that touches packet bytes. These are
written to be *checkable* — a reviewer should be able to point at a line and say it breaks
rule 4.

1. **One source of truth for length.** Every parser takes `(pointer, len)` where `len` is
   the number of bytes we actually have. A length read *out of* the packet may only be used
   after it is checked against the remaining bytes, and never to size a destination buffer
   separately from the copy that fills it. If a length appears twice, the code is wrong.
2. **No subtraction on an unvalidated length.** Check `a >= b` before writing `a - b`.
   Unsigned underflow is the single most common path to a huge `memcpy`. Prefer adding on the
   known-good side (`off + need <= len`) over subtracting on the attacker's side.
3. **Counters wider than the protocol allows.** If a field is 16 bits and the protocol lets
   a peer drive it to its maximum, the variable holding the running total is 32 bits, and the
   maximum is asserted. SACK Panic was a `u16` doing a `u32`'s job.
4. **Every loop over packet data has an explicit, bounded step counter**, not just a
   termination condition that depends on the packet being well-formed.
5. **Never follow a pointer that came from a packet.** `skip_name` in `net/dns.c` is the
   pattern to copy: a DNS compression pointer is *stepped over*, never followed, so no
   pointer loop is possible and no depth limit is needed. Where a protocol seems to require
   following one, that is a sign the feature should be dropped instead.
6. **Drop what we do not need, and say why.** No IP fragment reassembly (Ripple20,
   AMNESIA:33), no IP options, no source routing, no TCP urgent pointer, no IP-in-IP. Each
   of these has shipped a remote code execution in some stack. A feature with no user is an
   attack surface with no benefit — this is the no-bloat rule paying for itself.
7. **Nothing in the receive path allocates in proportion to attacker input**, and every
   table has a fixed size with a defined, non-exploitable eviction rule.
8. **No static scratch buffer in any function that can sleep.** See the `seg` bug below.
   A buffer shared between a sleeper and a waker is a correctness bug before it is a
   security bug.
9. **Rate-limit everything we emit in response to a received packet**, per peer, with
   jitter. Never with a single global counter an attacker can observe (SAD DNS,
   CVE-2016-5696).
10. **Every secret comes from a CSPRNG**, seeded at boot. Sequence numbers, ephemeral ports,
    DNS IDs, and later cookies. Nothing derived from `timer_ms()`.
11. **Fuzz before claiming correctness.** A parser that has not been fuzzed is untested, no
    matter how carefully it was read. See the plan below.

## What we do not implement, and are glad of

**There is no IPv6**, at any layer: no Ethertype 0x86DD handler, no IPv6 header or extension
header parsing, no ICMPv6, no neighbour discovery, no `AF_INET6` (`socket()` returns
`EAFNOSUPPORT`, which is what musl's resolver expects on a host without it). That is worth
stating in a security document rather than only in a feature list, because a large share of
AMNESIA:33 — including the IPv6 extension-header length bugs and picoTCP's ICMPv6 echo
integer underflow (CVE-2020-17443) — is unreachable code for us. When IPv6 does arrive,
extension-header chains are the single most dangerous part of it and belong under rules 1
through 6 above from the first line.

Also absent, deliberately: IP fragment reassembly, IP options, source routing, IP-in-IP
tunnelling, the TCP urgent pointer, SACK, and `listen`/`accept`. Every one of those has
shipped a remote code execution or a remote panic in some stack.

## Bugs found, and their state

Two passes over `net/`, `drivers/e1000.c`, `drivers/pci.c` and `net/socket.c` against the
rules above.

### Fixed

- **The static `seg` buffer in `net/tcp.c` raced with itself.** `emit()` filled the one
  static `seg[]`, then called `ip_send()`, which slept up to 1.5 s in `arp_resolve()` if the
  next hop was unknown (`net_sleep` → `wq_sleep_until` → `wq_sleep` yields the thread).
  Another thread reached `emit()`, overwrote `seg`, and the first thread then put the *other*
  connection's segment on the wire. Fixed by making `ip_send` non-blocking, so the bytes are
  copied out before it returns.
- **ARP resolution could never succeed from inside the receive path.** `net_rx_action()` has
  an `rx_busy` guard, so the `net_rx_action()` call inside `arp_resolve()`'s wait loop drained
  nothing and the reply sat in `rxq` for all three retries. Every such send stalled 1.5 s and
  failed. (The guard was doing its job — it is what kept this from being unbounded recursion
  and a kernel stack overflow. The bug was the blocking wait.) Fixed: a finished frame waits
  in a four-slot queue and the ARP reply sends it.
- **The ARP cache made that a remote denial of service.** `on_arp` recorded *any* ARP packet
  from anyone, including unsolicited replies, in an 8-entry table that evicted slot 0 when
  full — eight forged packets evicted the gateway. Fixed: the cache learns only from a reply
  to a request of ours, a request for our address is answered from the MAC already on the
  frame and never recorded, entries expire after 60 s, and a full table evicts the least
  recently used.
- **IP broadcast was accepted.** `on_ip` took `dst == 0xFFFFFFFF`, so a broadcast ICMP echo
  got a reply — the machine was a smurf amplifier, which is what Linux's
  `icmp_echo_ignore_broadcasts` exists to prevent — and a broadcast UDP datagram was handed
  to a bound socket with no way to tell it from a unicast one. Nothing needed it. Fixed:
  unicast to our address only, with a note for DHCP to carve out an exception later.
- **Our own ping could be answered by anyone.** `ping_got` was set by any ICMP echo reply
  whose sequence number matched, from any source address. Fixed: the reply must come from the
  host we pinged.
- **An oversized ICMP echo was dropped outright**, including for raw sockets, because the
  1400-byte bound on the reply buffer was applied to acceptance. Fixed: a large echo still
  reaches raw sockets, we just do not reflect it.
- **`drivers/e1000.c` leaked old packet bytes in short frames.** `TCTL_PSP` has the card pad
  to the 60-byte minimum out of a transmit buffer that is reused, and `e1000_xmit` copied
  only `skb->len` — so every ARP frame (42 bytes) carried 18 bytes of whatever last used that
  slot onto the wire. Fixed: the padding is zeroed.
- **DNS used a fixed source port (53000).** Any local program could `bind()` it and break
  every lookup the kernel makes, permanently. Fixed: an ephemeral port per lookup, which also
  removes one fixed quantity an off-path spoofer would otherwise not have to guess.
- **A DNS reply's question section was never matched against the question asked.** Fixed:
  `qdcount` must be 1 and the question section must byte-compare equal to what we sent
  (servers echo it verbatim). Note this checks the *question*, not the answer records — see
  `parse_a` under "Open", which is a separate and still-missing check.
- **Concurrent DNS lookups could swap answers.** `dns_got`, `dns_pkt`, `dns_wait_id` and
  `dns_len` are one shared set of globals, and `udp_bind` accepts a second bind with the same
  `ctx` (both `NULL`), so two callers would have silently crossed — resolving one name and
  being handed another's address. Only reachable from `net_test.c` today, but it is a
  security bug, not merely a race. Fixed with a one-at-a-time guard.

### Open

Needs the CSPRNG first — these are all one bug, "we have no randomness":

- **TCP ISN is `timer_ms() * 64000 + 0x10000000`.** The RFC 6528 attack verbatim: an off-path
  attacker who can reach us once predicts the next connection's sequence space and can spoof
  or reset connections blind. Wants a keyed hash over the four-tuple.
- **Ephemeral ports are a sequential counter from 49152**, in `net/tcp.c` and `net/udp.c`
  both. That is the other half of the four-tuple (RFC 6056).
- **The DNS transaction id is a counter.** With the port now random this is no longer the
  whole secret, but 16 predictable bits is not a secret either.
- **IP ID is a global counter**, which leaks how much traffic the machine sends.

Needs design or a feature, not just a fix:

- **`parse_a` does not check which name an answer belongs to.** It steps over each record's
  owner name with `skip_name` and takes the first `A`/`IN`/4-byte record it finds, whatever
  that record is actually about. So a response that passes the id, port, source-address and
  question checks can still carry an address for an unrelated name and we will use it. This
  is the response-matching rule of RFC 5452 §5 — the "bailiwick" check that the Kaminsky
  attack worked by evading. The practical attacker today is the configured resolver itself
  or someone who can spoof it, not a blind off-path one, but the check is missing.
  Not fixed yet on purpose: the naive version (require the owner name to equal the question)
  **breaks every CNAME**, because in a CNAME response the `A` record's owner is the chain's
  target, not the name asked about. Doing it right means accepting an owner that is either
  the question or the target of a CNAME already accepted in the same response, comparing
  compression pointers by value rather than following them (rule 5). That is a resolver
  change and wants its own step.
- **A forged reply can still win a race on a *solicited* ARP request.** The cache now only
  learns from a reply to a request of ours, which removes unsolicited poisoning entirely, and
  once an entry exists further replies are ignored until it expires. But in the window after
  our broadcast request, an attacker on the segment who answers faster than the real owner is
  cached. On a shared segment this cannot be fixed without authentication; it is narrowed,
  not closed.
- **Nothing stops us *sending* to a broadcast address.** `ip_send` applies `ip_ok` to the
  next hop, not the destination, so a datagram addressed to 255.255.255.255 goes out unicast
  to the gateway. Receiving broadcast is now blocked; sending is not. Asymmetric, and worth
  deciding on.
- **A local program can flush the four-slot ARP queue** by sending to many unresolved
  on-subnet addresses, evicting a queued TCP segment. TCP retransmits, so the cost is
  latency, and it needs local code execution.

- **Nothing we emit in response to a packet is rate limited**: ICMP echo replies, ARP
  requests, the RST for an unknown connection, the challenge ACK for a SYN on an established
  connection, and the ACK `tcp_input` sends for *any* data segment including a duplicate. One
  inbound packet becomes one outbound packet with no cap. When this is added it must be
  per-peer and jittered — a single global counter is an oracle (SAD DNS, CVE-2016-5696).
- **A peer that advertises a zero window stalls the connection forever.** `output()` computes
  `limit = min(snd_wnd, 4 * mss)`; with `snd_wnd == 0` the loop never runs, `arm()` is never
  called, so `rto_at` stays 0 and no timer fires. The data sits in the send ring and
  `tcp_write` has already told the caller it was accepted. Reachable from the SYN-ACK onward.
  Needs a persist timer and zero-window probes.
- **`SOCK_RAW` needs no privilege**, and `raw_icmp_input` hands every ICMP packet to every
  raw socket, so any program can both forge ICMP from this host and read other programs'
  ICMP. Linux requires `CAP_NET_RAW`; MyOS has no users or capabilities yet.
- **The neighbour cache never re-probes an entry in use.** A host that moves is unreachable
  until its 60 s expiry. Linux re-probes; we should too, once there is a timer hook for it.
- **`bind()` on a TCP or raw socket silently succeeds and does nothing** (`sys_bind` returns 0
  for anything that is not `SOCK_DGRAM`), so a program that binds a source port does not get
  it. `setsockopt` likewise accepts and ignores everything.
- **The parsers have never been fuzzed**, and no Spectre v1 barriers or retpolines exist on
  either target CPU.

### What this review does not cover

Stated so the list above is not mistaken for a clean bill of health.

- **It was reading, not testing.** Reading finds missing bounds checks, trusted lengths and
  predictable values reasonably well. It finds state-machine bugs — a sequence of legal-looking
  packets that drives TCP somewhere it should not go — badly. Those are what fuzzing and a
  state-space check find, and neither has been run.
- **The fixes are verified only against the happy path.** `make test` proves ping, DNS and
  TCP still work; no test sends a forged ARP reply, a broadcast echo, a zero-window SYN-ACK,
  a truncated DNS name, or a short frame to check the padding. Every fix here is believed
  correct and unproven. Attack-path self-tests are cheap to add — the loopback device and
  `netif_rx` can inject a crafted frame directly — and should come before any claim that a
  class of bug is closed.
- **Only `net/`, `drivers/e1000.c`, `drivers/pci.c` and the socket layer were reviewed.**
  The network stack is not the only place the kernel parses input it does not control. The
  **ext2 and page-cache code reads attacker-controlled bytes** off any disk or USB stick
  plugged in, with the same bug classes and none of this scrutiny; `copy_from_user` /
  `copy_to_user`, the ELF loader, `kmalloc`, signals and the TTY are all unreviewed. A
  hostile disk image is as real an attacker as a hostile packet.
- **Two passes found ten bugs, and the second pass found bugs the first missed.** That is the
  useful signal about whether the list is complete: it is not. A third pass would likely find
  more, and the fact that the second pass found the `parse_a` and zero-window issues after the
  first pass declared itself done is the reason to trust tooling over re-reading.

### Things that are already right, and should stay right

- `net/dns.c`'s `skip_name` steps over compression pointers instead of following them, and
  bounds its loop at 64 steps. That is the whole class of DNS name bugs in AMNESIA:33,
  designed out rather than patched. `parse_a` also validates every offset before reading.
- `on_ip` uses the IP header's `total` (after checking `total <= skb->len`) rather than the
  frame length, so Ethernet padding is never treated as payload.
- Dropping fragments and rejecting `ihl != 20` removes the Ripple20 bug class outright.
- Not sending RFC 5961 challenge ACKs for RSTs means we have no challenge-ACK side channel
  (CVE-2016-5696). Keep it that way: validate RSTs strictly, stay quiet.
- `net/socket.c` zeroes every `struct sockaddr_in` before copying it out, clamps every
  user-supplied length before using it, caps `msg_iovlen` at 16 against a fixed array, and
  bounds the per-socket datagram queue at 8. The socket layer was the cleanest part of this
  review: nothing found.
- TCP accepts in-order data only (`seq == c->rcv_nxt`) with no out-of-order queue, so there
  is no reassembly buffer to get wrong — the bug class that produced Ripple20.

## Plan

In rough dependency order. Each step should end with something testable, and anything on a
hot path needs a before/after from `make bench` per the performance rules.

- [ ] **A real CSPRNG first.** Everything else depends on it. ChaCha20 keyed from a boot-time
      entropy pool (TSC jitter, interrupt timing, the e1000 EEPROM, RDSEED only if CPUID
      says so — the P4 and Pentium M have neither RDRAND nor RDSEED). Then make `getrandom`
      actually cryptographic.
- [x] **The memory-safety, liveness and reflection bugs above** (see "Fixed").
- [ ] **Keyed ISN (RFC 6528)**, random ephemeral ports (RFC 6056), random DNS transaction
      ids, and per-destination IP IDs. All four are waiting on the CSPRNG and nothing else.
- [ ] **Per-peer rate limits with jitter** on every packet we generate in response to a
      received one. No global counters.
- [ ] **A TCP persist timer and zero-window probes**, so a peer cannot stall a connection by
      advertising a zero window.
- [ ] **Fuzz the parsers.** Build `on_ip`, `udp_input`, `tcp_input`, `parse_a` and the option
      parsers as a host-side harness and run them under AFL++ or libFuzzer with ASan and
      UBSan. These are pure `(pointer, len)` functions, which makes this cheap — the hardest
      part of fuzzing a kernel is already done by how this code is written. A corpus of the
      AMNESIA:33 and Ripple20 shapes (zero-length segments, lengths past the end, bogus
      option lengths, truncated DNS names) belongs in the repo.
- [ ] **Compiler and ABI hardening**, measured: `-fstack-protector-strong` (the Linux
      self-protection project's recommendation; about 20% of functions instrumented),
      `-fstack-clash-protection`, and `-fsanitize=bounds` with `-fsanitize-undefined-trap-on-error`
      in `make test` builds. Guard pages below kernel stacks — `docs/gaps.md` notes a stack
      overflow is currently a silent corruption and then a triple fault, which is the worst
      possible outcome.
- [ ] **Before `listen()`/`accept()` exists**: SYN cookies, designed in from the start, not
      added after a SYN flood takes the machine down. A listening socket is a much larger
      attack surface than a client, and everything above should be done first.
- [ ] **Capabilities for `SOCK_RAW`** once there are users (Linux: `CAP_NET_RAW`).
- [ ] **Spectre v1 barriers** on user-controlled indexes and retpolines; both target CPUs are
      affected. `docs/performance.md` has the Linux cost at under 1% of the UDP path in
      instruction count, but that must be measured on a real P4.

### A note on what the hardware can and cannot do

Worth deciding early, because it shapes everything above. **We cannot rely on the NX bit.**
On 32-bit x86 the no-execute bit is bit 63 of a page-table entry, so it exists only in the
PAE page-table format — and `docs/gaps.md` already records that execute permission is not
enforced without PAE. Worse, on our first target it may not exist at all: Intel's XD bit
arrived only in later Pentium 4 Prescott steppings (the 5x0J, 5x1, 5x5J and 5x6 models), and
the first Pentium M ("Banias") supports PAE but does not even report it in CPUID.

So the baseline defence has to be software: bounds-correct parsers, stack canaries, guard
pages, and W^X enforced through the page tables where we can. Detect XD via CPUID and use it
when present, but never let it be the thing standing between an attacker and the kernel.
This is the opposite of how a modern x86-64 kernel is allowed to think, and it is why rules
1 through 5 above are not optional.

Open questions:

- Where does TLS live? User space, almost certainly (no crypto in the kernel beyond the
  CSPRNG) — but then what does `initramfs/etc/` hold for trust anchors, and who updates them?
- Do we want a packet filter at all, or is "drop everything we have no user for" enough?
  Leaning on the second: it is less code and less to get wrong.
- Is DNSSEC validation in scope, or do we rely on random IDs plus ports and accept that a
  same-segment attacker wins? (Probably the latter, with TLS above it doing the real work.)
