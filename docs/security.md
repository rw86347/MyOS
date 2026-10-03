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
  (servers echo it verbatim).
- **`parse_a` did not check which name an answer was about.** It stepped over each record's
  owner name and took the first `A`/`IN`/4-byte record it found, whatever that record was
  actually about — so a reply that passed the id, port, source-address and question checks
  could still carry an address for an unrelated name. This is the response-matching rule of
  RFC 5452 §5, the check the Kaminsky attack worked by evading.

  Fixed, and it needed more than a byte compare. Requiring the owner name to equal the
  question **breaks every CNAME**, because there the `A` record is about the end of the chain
  and not about the question at all. So `parse_a` now walks the chain: a record counts if its
  owner is the question, or the target of a CNAME that already counted, up to `CNAME_MAX`
  hops. Records about anything else are skipped rather than trusted, and skipped rather than
  treated as the end of the list — a forged record first in the section must not hide a real
  answer behind it.

  Comparing names means resolving compression, and `name_expand` is the one place a pointer is
  followed rather than stepped over. Following them is exactly how CVE-2020-24338 hung a stack
  forever. It is safe here because **a pointer must land strictly further back than the last
  one did**: the targets strictly decrease, so a cycle cannot be built at all, and a hop limit
  keeps the cost linear instead of quadratic in the message length. Names are lowercased
  first, because a name ignores case on the wire (RFC 4343) and a server may well answer
  `EXAMPLE.com` — rejecting that would have broken real lookups while looking like security.
- **A peer that advertised a zero window stalled the connection forever.** `output()` computes
  `limit = min(snd_wnd, 4 * mss)`; with `snd_wnd == 0` the loop never ran, so `arm()` was never
  reached, so `rto_at` stayed 0 and **no timer was left running at all**. The data sat in the
  send ring, `tcp_write` had already told the caller it was accepted, and nothing would ever
  happen again — not a retransmit, not a timeout. A blocking `tcp_write` or `tcp_flush` waited
  in `nap()` for good. Reachable from the SYN-ACK onward, by a peer that need only set two
  bytes to zero.

  This one needed a missing feature rather than a corrected check, and the feature is the
  persist timer of RFC 1122 §4.2.2.17. `output()` now lets **one byte** past a shut window.
  The peer must drop a byte that is past its window but must answer with its current window,
  which is both how the window reopening is noticed when the update announcing it was lost,
  and how we learn the peer is still alive. Because that byte counts as sent, the ordinary
  retransmit timer repeats it, and that repetition *is* the persist timer — no second timer,
  no new state in `struct tcp_conn`.

  Two things do not fall out for free. A shut window is **not** a lost packet: the peer is
  alive and its application has stopped reading, which can legitimately last minutes, so
  probing gets a longer leash (`PROBES`) and backs off further (`PERSIST_MAX`) than
  retransmission does — about four minutes rather than twenty seconds. And a window that
  reopens must **clear** that backoff, because `take_ack` deliberately ignores an ack that
  acknowledges nothing new, which is exactly what a pure window update is. Without that,
  probes stayed charged against the connection and the next timer killed a connection that
  had just become healthy, after a minute of crawling. That second half is a bug the fix
  introduces if written carelessly, so `make attacks` tests it separately.

  Deliberate deviation: RFC 1122 says to keep a connection open for as long as the peer
  answers probes. MyOS gives up after `PROBES`. Code that never gives up is a hang, and a hang
  is what the attacker wanted — see `docs/gaps.md`. A connection whose socket has been
  released is capped much tighter, at `ORPHAN_MS` (10 s) by `end_at`, so the longer leash
  cannot be used to hold a connection slot after the application is done with it.
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
- **`SOCK_RAW` needs no privilege**, and `raw_icmp_input` hands every ICMP packet to every
  raw socket, so any program can both forge ICMP from this host and read other programs'
  ICMP. Linux requires `CAP_NET_RAW`; MyOS has no users or capabilities yet.
- **The neighbour cache never re-probes an entry in use.** A host that moves is unreachable
  until its 60 s expiry. Linux re-probes; we should too, once there is a timer hook for it.
- **`bind()` on a TCP or raw socket silently succeeds and does nothing** (`sys_bind` returns 0
  for anything that is not `SOCK_DGRAM`), so a program that binds a source port does not get
  it. `setsockopt` likewise accepts and ignores everything.
- No Spectre v1 barriers or retpolines exist on either target CPU.

## Fuzzing the parsers

`tools/fuzz/` builds the five `net/` sources with the **host** compiler and links them against
`tools/fuzz/stubs.c` instead of the rest of the kernel. The point is AddressSanitizer: on the
host a one-byte overrun in a parser is an immediate, located failure, where on real hardware
or under QEMU it is silent corruption that boots fine. The kernel sources are compiled
unmodified — only `interrupts.h` and `uaccess.h` are replaced, because they are inline i386
assembly — so anything the fuzzer finds is a bug in the kernel and not in a rewritten copy of
it.

    make attacks           assert that attacks are rejected (run this after touching net/)
    make fuzz              fuzz all five targets
    make fuzz T=dns N=1e7  one target, longer
    make fuzz-replay       re-run every saved input
    make fuzz-coverage     what the fuzzing actually reached, as source lines

Apple's clang ships the sanitizer runtimes but not libFuzzer, so `tools/fuzz/driver.c` is the
mutation loop: a corpus, a mutator that knows lengths on the wire are big-endian, and
`-fsanitize-coverage=trace-pc-guard` for feedback. An input that reaches an edge no earlier
input did is kept. Build with `FUZZER=libfuzzer` to use the real one where it exists; the
targets are plain `LLVMFuzzerTestOneInput` either way.

Three things make it find bugs rather than merely run: the sanitizers turn an overrun into an
abort, a death callback saves the input that did it, and an alarm fires if one single input
takes 30 seconds — because a parser that never returns is a bug too. That last one is aimed
squarely at CVE-2020-24338, a DNS name that pointed at itself.

### Getting past the gates

The hard part is not generating bytes, it is reaching the code. A random 40-byte string fails
the IPv4 header checksum essentially always, so a naive fuzzer watches `on_ip` drop packets at
line four forever. Each target therefore repairs the fields that are pure bookkeeping and
leaves alone everything an attacker controls:

| target | what it drives | what the harness repairs |
|---|---|---|
| `wire` | `on_ip`, raw | nothing — the drop paths and length checks |
| `ip`   | ICMP echo, dispatch into UDP and TCP | version, header length, our address, checksums |
| `arp`  | `on_arp`, with a request outstanding so `remember` is reachable | nothing |
| `dns`  | a reply to a lookup the stack is really waiting for | transaction id, question section |
| `tcp`  | a sequence of segments against a live connection | ports, checksum, optionally seq/ack |

The `dns` and `tcp` targets are the interesting ones. Both let the stack send first, read the
transaction id or sequence number it chose out of the frame, and put it in the reply — so the
bytes reach `parse_a` and the TCP state machine instead of being rejected at the door. For TCP
that models an attacker who can see our traffic, which is normally much stronger than an
off-path one; here the ISN is `timer_ms() * 64000` plus a constant, so an off-path attacker can
work it out anyway. It is the threat model, not a generous assumption.

Each repair is a check no longer being tested, which is why `wire` exists and repairs nothing.

### Result

145 million runs across the five targets: **no crash, no sanitizer report, no hang.** What
makes that worth saying is the coverage it came with, from `make fuzz-coverage`:

| function | lines | branches |
|---|---|---|
| `on_arp` | 100% | 100% |
| `udp_input` | 100% | 92.9% |
| `skip_name` | 100% | 85.7% |
| `on_ip` | 98.3% | 97.2% |
| `name_expand` | 96.9% | 87.5% |
| `parse_a` | 93.8% | 87.0% |
| `encode_name` | 89.5% | 70.0% |
| `tcp_input` | 85.9% | 83.3% |

By file: `net/dns.c` 90.6%, `net/udp.c` 84.5%, `net/ipv4.c` 84.3%, `net/tcp.c` 59.2%. The TCP
number is low because most of that file is the send path — `tcp_write`, `tcp_read`, the
retransmit queue — which a received packet does not reach; the part that parses what arrives is
covered at 85.9%.

Coverage stopped moving early: the first 25 million runs reached every line the next 100 million
did. That is the signal to stop adding runs and start adding reach — the way to find more is a
target that drives code these five do not, not a longer campaign on the same five. `remember`
at 63.6% is the clearest example, because the lines being missed are the cache's LRU eviction,
which needs more neighbours in play than any target creates.

So "the parsers have been fuzzed" is now a true statement about `on_ip`, `on_arp`, `udp_input`,
`parse_a` and `skip_name`, and a mostly-true one about `tcp_input`. It is **not** a statement
that they are correct:

- Coverage is not correctness. A line can execute and still be wrong, and nothing here checks
  that the stack produced the *right* answer — only that it did not crash, hang, or corrupt
  memory. The `parse_a` flaw is the proof: the `dns/foreign-a` seed made the resolver return an
  address from an answer about `evil.example` when it had asked about `example.com`, and every
  one of 125 million runs passed, because accepting the wrong address is not a memory error.
  It took `make attacks` to catch it.
- `irq_save`/`irq_restore` are nothing on the host, so a bug that needs an interrupt to land
  between two instructions is invisible here. Races remain a reading problem.
- The ARP pending queue and the neighbour table are global and a run can leave a mark on the
  next one, so a saved crash might not reproduce under `-replay` on its own.
- The fuzzer only ever sends *one* packet per run to `wire`, `ip` and `arp`. Bugs that need a
  particular sequence are only reachable on the `tcp` target today.

## Testing that attacks are rejected

`make attacks` is the other half, and the half that matters more. It performs real attacks and
asserts the behaviour that is supposed to stop them. It shares the stubs and the sanitizers with
the fuzzer and asks a different question: not "did the stack survive" but "did it refuse".

Every fix in this document should have an entry, and **a fix with no entry here is a fix nobody
has tested**. 40 checks, about two seconds, so there is no excuse for not running it after
touching `net/`:

| area | what is asserted |
|---|---|
| ARP | an unsolicited reply cannot overwrite the gateway's entry, or create one; a reply with a multicast or all-zero sender MAC is refused; a request for our address is answered but teaches the cache nothing; a request for someone else's address is ignored |
| ICMP | an echo to broadcast, subnet broadcast, multicast or another host draws no reply (no smurf amplifier); an echo too big for the 1400-byte reply buffer is not reflected; a fragment is dropped |
| our ping | an echo reply from a host we did not ping does not satisfy the ping |
| UDP | a datagram to an unbound port or to port 0 draws no reply (no reflector) |
| TCP | an out-of-window reset is ignored (RFC 5961) but an in-window one is accepted; data for an unknown connection draws a reset |
| TCP windows | a shut window is probed rather than waited on, and the probe is one byte and not the whole queue; a window shut and then gone silent eventually gives up; a window that reopens delivers the data and clears the probe backoff |
| DNS | a reply is refused if the transaction id, the question, the source address or the source port is wrong; an answer about a different name is refused (RFC 5452) but a foreign record does not hide a real one behind it; a CNAME chain is followed; a CNAME loop and a self-referential compression pointer both terminate and fail; a name differing only in case is accepted |

Half of those are positive controls, deliberately. A test that asserts "no reply was sent"
passes perfectly when the stack is broken and never replies to anything, so every "this must be
ignored" has a "this must work" beside it: ping to our address *is* answered, a 1300-byte echo
*is* reflected, a legitimate reset *does* close the connection, a well-formed DNS reply *does*
resolve. Writing them caught two bugs in the tests themselves, both the same mistake — draining
the send queue before looking at it, which made "nothing was sent" trivially true.

The other way a test lies is by passing against the broken code, so each new check is run once
with the fix reverted and has to fail. That is not a formality. The first version of "a reopened
window clears the probe backoff" passed either way: it advanced the clock by one ordinary
retransmit interval, and the stale deadline the bug leaves behind is a *minute* out, so the
timer it was trying to provoke never fired. The test was measuring nothing.

The suite also carries open bugs as **known** entries: the test states the behaviour we want, so
it reports rather than fails, and the day it starts passing the suite prints `FIXED` and the
entry gets promoted to a real check. That is how an open bug in this document stays visible
instead of being forgotten — and it worked: `parse_a` was entered that way, and the suite
announced the fix on the run after it landed. There are no `known` entries left today.

### What this review does not cover

Stated so the list above is not mistaken for a clean bill of health.

- **The review itself was reading, not testing.** Reading finds missing bounds checks, trusted
  lengths and predictable values reasonably well. It finds state-machine bugs — a sequence of
  legal-looking packets that drives TCP somewhere it should not go — badly. Fuzzing now covers
  the memory-safety half of that (see below); the half that asks whether the stack produced the
  *correct* answer is still unchecked.
- **One fix is still unverified: the Ethernet padding.** `make attacks` now covers the rest
  (see below), but `drivers/e1000.c` is not in that build — it needs PCI, MMIO and interrupts —
  so nothing checks that a frame shorter than 60 bytes is padded with zeros rather than with
  whatever the previous packet left in the transmit buffer. Doing it properly means capturing
  the wire, which QEMU will do with `-object filter-dump`.
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
- [x] **A TCP persist timer and zero-window probes**, so a peer cannot stall a connection by
      advertising a zero window (see "Fixed").
- [x] **Fuzz the parsers** — done, see "Fuzzing the parsers" above. `on_ip`, `udp_input`,
      `tcp_input` and `parse_a` are pure `(pointer, len)` functions, which made it cheap: the
      hardest part of fuzzing a kernel was already done by how this code is written. The
      AMNESIA:33 and Ripple20 shapes are in the seed corpus.
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
