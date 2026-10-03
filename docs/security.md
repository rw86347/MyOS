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

- **Four numbers an attacker could predict, which were all one bug: no randomness.** A TCP
  initial sequence number, an ephemeral port, a DNS transaction id and an IP ID were each a
  counter or a function of the clock, and between them they are most of what the RFC 6528,
  RFC 6056 and Kaminsky attacks need. Fixed by building the generator they were all waiting
  on — see "The CSPRNG" below — and then:

  - **TCP ISN was `timer_ms() * 64000 + 0x10000000`**, the same number for every connection
    opened in the same millisecond, from a clock the attacker can read by other means. Now
    `M + F(four-tuple, secret)` per RFC 6528: `M` is a 4 µs timer so numbers still climb with
    time the way a peer holding a stale connection expects, and `F` is a keyed hash so the
    offset is unrelated for every four-tuple. Knowing the sequence space is what lets an
    off-path attacker inject into or reset a connection blind.
  - **Ephemeral ports were a counter from 49152** in both `net/tcp.c` and `net/udp.c` — the
    other half of the four-tuple (RFC 6056). Now chosen at random from 49152..65535 by one
    shared `ip_ephemeral_port`, deliberately *not* a random start followed by a scan, because
    with a scan knowing one port tells you where the next will be.
  - **The DNS transaction id was a counter.** 16 bits, and one subtraction from free after a
    single observed lookup. That is the arithmetic the Kaminsky attack ran on. Now random.
  - **IP ID was one counter for the whole machine**, which answered a question nobody should
    be able to ask: send us two packets, subtract the ids, and you know how many packets the
    machine sent to everyone else in between. That is a traffic meter on the host and the
    basis of nmap's idle scan. Now one counter per destination, in 64 buckets chosen by a
    hash with a boot-time secret, each starting at a random value. The hash is a multiply and
    a shift rather than a call into the CSPRNG, because this one runs for every packet sent.

  `getrandom()` and `AT_RANDOM` were on the same xorshift. `AT_RANDOM` is what musl builds its
  stack guard value from, so that was a predictable canary.

- **Nothing we sent in response to a packet was rate limited**: ICMP echo replies, ARP
  replies, the reset for a connection that does not exist. One inbound packet became one
  outbound packet with no cap, which makes the machine a reflector — the source address is
  the attacker's choice, so the reply goes wherever they point it.

  Fixed with a token bucket, 64 packets per second with a burst of 64, in 16 slots. Three
  things about it matter more than the numbers. It is keyed on **where the reply is going**,
  so the protected party is the victim being aimed at; an attacker who spoofs a thousand
  sources gets a thousand buckets but each reply lands somewhere different, so there is no
  flood to aim. It is **per peer** rather than one counter, because a global counter is itself
  an oracle — watching it run out tells an attacker that someone else is talking to us, which
  is the side channel behind CVE-2016-5696 and SAD DNS. And the budget is **jittered**,
  because a cutoff at exactly N packets is a clean edge to measure; with it moving, the
  measurement returns noise.

  Residual weaknesses, written down rather than hidden: two peers can land in the same slot,
  and an attacker who can spoof source addresses can push a victim's slot out of the table.
  Both are better than no limit and neither is fixable without per-peer state we cannot
  afford. The normal ACK for in-window data is deliberately *not* limited: that one is
  required for TCP to work, not a reflector.

- **A raw socket could send any ICMP message type.** That is a weapon aimed at the rest of the
  network rather than at this machine: an ICMP redirect reroutes a neighbour's traffic, and a
  forged "fragmentation needed" or "destination unreachable" tears down connections between
  two other hosts. Linux keeps it behind `CAP_NET_RAW`.

  MyOS has no users, so there is nobody to withhold a capability from — but there is also
  only one program that wants a raw socket, and `ping` sends exactly one thing. A raw socket
  may now send an echo request and nothing else, which removes the attack rather than gating
  it. `-EPERM` otherwise, which is what Linux returns without the capability.

### The CSPRNG

Worth its own section, because everything above depends on it and because the hard part is
not the cipher.

**The stream is ChaCha20**, the same construction Linux's `/dev/urandom` uses, and it suits
this machine: 32-bit adds, xors and rotates and nothing else — no tables to miss in the
cache, no multiplies, and no SSE, which matters because the kernel is built
`-mgeneral-regs-only` and may not touch the FPU registers at all. It runs **fast key
erasure**: every block of output also produces the next key and the old one is overwritten,
so an attacker who reads kernel memory cannot work backwards to bytes already handed out.
A known-answer test against the RFC 8439 vector runs at every boot in `make test` — a broken
ChaCha20 still produces output that looks perfectly random, so no statistical check would
catch a wrong rotation, and only the published answer will.

**The entropy is the hard part, and it is weak.** Both target CPUs predate RDRAND and RDSEED
by years, so there is no hardware generator to ask: it has to be measured. What is measurable
is how long things take. A read of port 0x80 is an ISA bus cycle, and the cycle counter says
how long it really took — a number that is not constant on real hardware, because DRAM
refresh, bus contention, cache and TLB state and SMIs the operating system never sees all
move the low bits. We keep the *second* difference, the change in the interval rather than the
interval, because the interval has a large predictable part and only its variation is
unpredictable. 512 samples, folded into a rotating pool. Linux does the same thing in
`try_to_generate_entropy()` when it has nothing better.

Be clear about what that is worth. Each sample is perhaps half a bit to two bits, so the honest
claim is "probably enough for a 128-bit key on real hardware", not a measured figure. And
**under QEMU with `-icount` the cycle counter is deterministic, so on the emulator this
produces the same pool every boot** — the numbers only become real on real hardware, which is
the machine that matters. The RTC's epoch and the cycle counter are mixed in for uniqueness
rather than strength, so two machines booting the same image cannot agree. Every block also
mixes the current cycle counter into its nonce, so timing variation accumulated since boot
keeps contributing.

Nothing here is on a hot path: every caller wants a few bytes once per connection or once per
lookup, and the one per-packet value, the IP ID, uses a keyed counter instead. Cost at boot,
measured: 18 µs of CPU work in QEMU (1093 → 1111 µs to kernel ready). On real hardware expect
nearer half a millisecond, because an ISA port cycle there is about a microsecond rather than
QEMU's software handling of one — under 2% of a boot that spends 27 ms waiting for disks.

### Found by auditing the rest of the kernel (2026-10-03)

The network stack had been gone over several times; these came from pointing
the same method at the filesystem, the syscall layer and the drivers. Two
threat models: a crafted disk image (a USB stick — `storage_init` auto-mounts
an ext2 volume labelled `myos-root`/`myos-data` read-write, so plugging it in
is the whole attack), and a hostile unprivileged program.

**The one that changes how the kernel has to be read: `copy_to_user` and
`copy_from_user` can sleep.** `arch/i386/interrupts.c` offers every page fault
to `mm_fault` first, including a ring-0 fault inside `__copy_user`, and for a
file-backed mapping that goes `mm_fault` → `file_copy_out` → `pcache_read` →
`wq_sleep` on the disk. ext2 is the root filesystem, so `mmap`ing a file is the
ordinary case. A syscall is therefore **not** atomic, and "syscalls run with
interrupts off, so they never race" — true as far as it goes, and relied on in
`net/netdevice.c` — does not cover a copy to or from user memory. Interrupts
being off only closes the windows that contain no copy.

Three ring buffers were written against the stronger reading: they computed how
much room or data there was, copied, and committed `head`/`tail`/`len`
afterwards. A `fork` shares pipes and sockets, so two processes could both act
on the same snapshot, both commit, and drive an unsigned difference below zero —
after which it stops clamping and the next call copies with a length near 2^32:

- `fs/pipe.c` — `head` passes `tail + PIPE_SIZE`, `room` underflows, and the
  next `write` runs off the end of a one-page buffer with as much
  attacker-chosen data as it likes. Now the space is claimed before the copy,
  and a failed copy leaves zeros rather than recycled kernel memory.
- `net/socket.c` — the datagram was freed by whichever reader finished first
  while the other kept reading and then freed it again, taking `s->count`
  below zero, which makes `wait_rx` report data on an empty queue and the next
  read dereference NULL in ring 0. The datagram is now taken out of the queue
  before it is copied, so there is one owner; `MSG_PEEK` puts it back.
- `net/tcp.c` — the same shape on both rings: `tx_len` past `TXBUF` gave an
  unbounded write into the connection's 16 KiB buffer, `rx_len` below zero gave
  an unbounded read of kernel heap handed to user space. Both are now clamped
  as well as committed first, so even a raced count cannot leave the array.

**ext2, against a crafted image.** Seven out-of-bounds accesses, all from
believing a field on the disk:

- `rec_len` of the `.` entry was used unchecked as the offset of `..` when a
  directory was renamed into a new parent: a 4-byte write at any offset up to
  64 KiB past a one-page buffer, with a value also taken from the disk. The
  cleanest primitive of the set.
- `name_len` was never checked against `rec_len`, so an entry at the end of a
  block with `rec_len` 8 and `name_len` 255 read 255 bytes out of the next page
  and `getdents64` printed them. Three walks shared the bug; all three now go
  through one `dirent_ok`.
- `inode_size` was range-checked but not required to be a power of two, so an
  inode could straddle the end of its block — a disclosure through `stat` and
  `readlink` on the read side, and a `memset` past the page on the write side.
- `inodes_per_group`/`blocks_per_group` were only checked non-zero, but one
  bitmap is one block: a larger value walks `take_bit`/`clear_bit` off the page
  and sets a bit in whatever follows.
- A fast symlink holds 60 bytes in `block[]`, but `size` up to `PATH_MAX` was
  reported, so `readlink` returned up to 196 bytes of unzeroed `kmalloc` heap.
- `load_symlink`'s failure was ignored, leaving `data` NULL and `size`
  non-zero; `ls -l` on the directory then dereferenced NULL in ring 0.
- `blocks_count` of zero divided by zero in the mount message — a `#DE` in
  ring 0 before userspace exists, so the machine is dead at boot.

**An unprivileged program could halt the machine with one syscall.**
`rt_sigreturn` restored `gs`/`fs` from the signal frame — which the program
writes — checking only that the low bits said ring 3. Those registers are
reloaded by `pop gs` on the way out, which still runs at CPL 0, where the CPU
requires DPL >= 3; `0x0B` is the kernel code segment with ring-3 bits set, so
it raises #GP *inside the kernel*, which is fatal. `gdt_user_seg_ok` now checks
the live descriptor — present, ring 3, not a system segment, readable if it is
code — which also gets the case of a TLS slot that `set_thread_area` has not
filled in yet, where a whitelist of indices would still have faulted.

**`/dev/zero` had no `RW_MAX`.** Every real filesystem caps one `read` at 256
KiB precisely because a syscall cannot be preempted; `read(fd, buf, 0x7fffffff)`
on `/dev/zero` held the CPU for two million copies and the 1 ms slot with it.

**And one in code written an hour earlier.** `sys_getrandom` looped over the
whole request with no interruption point, so a gigabyte of valid buffer meant
millions of ChaCha20 blocks inside one syscall. It now checks for a pending
signal every 4096 bytes and returns a partial count, which is what
`getrandom()` promises and what Linux does.

### Open

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

- **`SOCK_RAW` still needs no privilege, and a raw socket still reads every ICMP packet.**
  Sending is now restricted to echo requests (above), which removes the attack on other
  hosts. What remains is that any program can open a raw socket at all, and that
  `raw_icmp_input` hands every ICMP packet to every raw socket that is not connected — so one
  program can watch another's ping replies and the ICMP errors belonging to its connections.
  Linux requires `CAP_NET_RAW` for this. **It cannot be fixed here until there are users:**
  `sys_getid` returns 0 for every id and `setuid` is a stub, so a capability check would be
  granted to everything and would only look like security. The right place for it is
  `raw_send_ok` in `net/socket.c` and the `SOCK_RAW` branch of `sys_socket`.
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

The CSPRNG made reproducibility harder and the harness had to be changed for it. The real
`kernel/random.c` is compiled into the fuzz build — a stand-in would mean the tests were not
exercising the generator the kernel actually uses — against a cycle counter that counts rather
than measures, so the stream is the same on every run. But the generator carries its key
forward, so without re-keying the stream would still depend on how many runs had gone before,
and the rate limiter now *drops* packets on an unpredictable budget, which would quietly stop
a saved crash from reproducing. So each run re-keys. That costs about 40% of the throughput
(167k to 100k runs/s on this Mac) and is worth it: a crash that cannot be replayed is a crash
that cannot be fixed.

## Testing that attacks are rejected

`make attacks` is the other half, and the half that matters more. It performs real attacks and
asserts the behaviour that is supposed to stop them. It shares the stubs and the sanitizers with
the fuzzer and asks a different question: not "did the stack survive" but "did it refuse".

Every fix in this document should have an entry, and **a fix with no entry here is a fix nobody
has tested**. 51 checks, about two seconds, so there is no excuse for not running it after
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
| unguessable numbers | sequence numbers of connections opened in the same millisecond are unrelated; local ports and DNS transaction ids and DNS source ports are not counters; a port stays inside the ephemeral range; IP IDs count for one destination but are not one sequence across several |
| rate limiting | a flood of echoes and a flood at closed ports are both cut off; one peer's flood does not silence another peer; the budget comes back with time |

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

`make verify-tests` does that check mechanically. It reintroduces one bug at a time by patching
the source — the clock-derived ISN, the port counter, the DNS id counter, the machine-wide IP
ID, each rate limit removed, the rate limit made global — rebuilds, and reports which checks
noticed. Seven bugs, and each must be caught by its own test and not by a different one. It
caught a real problem the first time it ran: three cases reported the *wrong* test failing,
because a patch and a build inside the same second leave `make` thinking the binary is current,
so the previous case's binary was answering for the next one. It deletes the objects now.

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
  hostile disk image is as real an attacker as a hostile packet. **ext2 now has a harness of
  its own** (see below) and ten bugs came out of it; FAT32, the ELF loader and the TTY still
  have nothing.
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

## Fuzzing a hostile disk image

The section above ends by admitting that the review covered `net/` and not `fs/`, and that
**a hostile disk image is as real an attacker as a hostile packet**. It is in fact a worse
one. `storage_init` mounts anything labelled `myos-root` or `myos-data` automatically, and
mounts it read-write, so plugging in a USB stick is the entire attack. There is no port to
be closed, no socket to be unbound, and the parser runs before any user exists to be denied.

`make diskattacks` and `make diskfuzz` are the same trick as the network harness, pointed at
`fs/`: build the real `fs/ext2.c`, `fs/bcache.c`, `fs/pcache.c`, `fs/partition.c` and the VFS
on the host against `tools/fuzz/diskstubs.c`, under AddressSanitizer and UBSan, hand them a
block device backed by a byte array, and mount it. The kernel sources are compiled unmodified,
so a bug found here is a bug in the kernel.

`tools/fuzz/disktarget.c` is what gets run per image: scan partitions, mount, walk every
directory, read every file, read every symlink, then *write* — create and truncate and unlink,
make two directories and rename one into the other (which is the path that rewrites `..`),
symlink — and walk again. 30 deterministic cases in `tools/fuzz/diskattacks.c` corrupt one
specific field each; `make diskfuzz` then mutates bytes at random, biased towards the
superblock, the group descriptors, the inode table and the two known directory blocks.

Each case and each run is a **fork**. The point is to survive a crash and name it rather than
die on the first one, and ASan's report in the child is the parent's evidence. Coverage lives
in `mmap(MAP_SHARED)` so the parent can tell a new code path from a repeat and keep the input
that found it; kept inputs are written to `tools/fuzz/corpus/disk/` as **deltas against the
base image** — an image is 4 MiB, a delta is about a hundred bytes, and `git show` on one
tells you what it does.

### Making the harness able to see

Most of the work was not writing cases, it was making the harness *capable of noticing*, and
this is the part worth remembering:

- **The base image must use 4 KiB blocks.** `bcache` gives every buffer a whole page from
  `pmm_alloc`. With 1 KiB blocks there are 3072 bytes of slack after each block, so a
  four-byte write just past the end lands in memory that is validly mapped and nothing
  complains. The image is generated with `mke2fs -b 4096` precisely so that a block *exactly
  fills* its allocation and the next byte is in a poisoned redzone.
- **`kmalloc` maps to plain `malloc`, not to a size-class allocator.** A size class rounds
  100 bytes up to 128 and hides a 20-byte overflow completely.
- **Fresh `kmalloc` memory is filled with `0xA5`, not zeroed.** This catches the class of bug
  no sanitizer can see: memory that is validly allocated but never written, handed to user
  space. The fast-symlink disclosure is found by looking for `0xA5` coming back out, not by
  any memory error.

### What it found

Six of the first eight cases were measuring nothing — all for the slack-space reason above —
which `make verify-disktests` caught. That script reintroduces each of the eleven ext2 fixes
one at a time, rebuilds, and requires that the matching case, and not a different one, fails.
It is the only reason to believe any of this.

Then the fuzzer found three bugs in its first minutes that two careful readings of the same
code had missed:

| bug | why reading missed it |
|---|---|
| `rec_len` was not required to be a multiple of four, so a walk read a `uint16_t` at an odd address | undefined C, and completely harmless on x86 — but the original Raspberry Pi is a roadmap target, and an unaligned halfword load there is not something to rely on |
| the `..` fixup in `rename` placed a four-byte **write** at `"."`'s `rec_len` without requiring it to be aligned either | its *bounds* had just been fixed, in this same round. Fixing one of two properties of a value and not noticing the other is not something re-reading the patch reveals; the saved corpus replayed it straight away |
| `fs->groups` divided by `blocks_per_group` three lines **above** the check for `blocks_per_group == 0` | the check existed, was correct, and was written in the same sitting; nothing in reading the two statements separately reveals the order. A zero divisor is `#DE` in ring 0: the machine is dead at mount, before user space |

The last is the useful one to generalise from. The rule it violates is not "check your
inputs" — that was done — it is **nothing may be derived from a superblock field before that
field has been checked**, and that is an ordering property, which is exactly what reading
code is bad at and what running it is good at. The validation block now sits above every
derived value, with a comment saying why it has to.

An eighth ext2 bug had already come out of writing the harness: the message that *rejects* an
out-of-range `log_block_size` printed `1024 << log`, shifting by the very value it had just
decided was too large.

### Two ways the harness lied, both now closed

Both are worth recording, because they are failures of the thing that checks for failures.

- Piping `make verify-disktests` into `head` killed it with SIGPIPE **before its restore
  trap ran**, leaving a deliberately reintroduced bug in the tree. Both verify scripts
  ignore SIGPIPE now.
- A patch whose target text had moved no longer applied — and `run_case` went on to rebuild
  the *unmodified* file, watch every case pass, and report `NOT CAUGHT`. A script written to
  catch tests that measure nothing, measuring nothing. It reports `PATCH DID NOT APPLY` now.
  It also **deletes the binary on exit**, because the one it used to leave behind was built
  from the last reintroduced bug, and a fuzz session started afterwards duly "found" it.

### What this still does not cover

- **FAT32 has no target yet.** `fs/fatvfs.c` parses a boot sector, a FAT and directory
  entries from the same untrusted disk and has had no scrutiny at all. It is the obvious next
  one, and the harness is already built for it.
- **The write paths are exercised but barely steered.** Mutations aim at the superblock and
  the inode table because that is where structure lives; the allocator bitmaps and the
  indirect-block chain are reached only by luck so far.
- **No long run yet.** The numbers in this section come from minutes, not hours.

## The copies that sleep, and the audit that followed

This is the most load-bearing thing in the document, so it comes with its own
section. The claim the kernel was built on is written out in `net/netdevice.c`:
a syscall is atomic, because interrupts are off for all of it. The `int 0x80`
gate really is an interrupt gate (`0xEE`, and type `0xE` clears IF) and
`sysenter` really does clear IF, so the claim looked safe.

**It is false, and it is false in the ordinary case rather than an exotic one.**
`copy_to_user` and `copy_from_user` can sleep. A fault on a page of an `mmap`'d
file goes to `mm_fault`, which asks the page cache, which waits for the disk
and switches to another thread. ext2 is the root filesystem. Nothing unusual
has to happen.

So the shape below is a bug, and `fork` or `dup` or `clone(CLONE_THREAD)` is
all an attacker needs to reach it, because each of those gives two processes a
handle on the same object:

```
    room = SIZE - len;          /* 1. measure the shared state */
    n    = min(len_wanted, room);
    copy_to_user(buf, ring + at, n);    /* 2. ...and sleep in the middle */
    head += n;                  /* 3. commit, from a stale measurement */
```

Both callers measure the same `room`, both copy, both commit. The index passes
the end. `room` is unsigned, so the next call computes a negative number as
something near four billion, **the clamp stops clamping**, and the call after
that copies with a length limited only by the buffer the caller asked about.
That is an out-of-bounds kernel read handed to user space, or an out-of-bounds
kernel write, depending on direction.

### What was found

Seven sites, in two passes. The first four were found by looking for the shape;
the last three by a second pass that worked from **shared mutable state**
instead — enumerate the objects two processes can hold at once, then ask what
happens if a copy sleeps in the middle. That framing is why the second pass
found what the first missed, and it is the one to use next time.

| where | consequence |
|---|---|
| `fs/pipe.c` `pipe_read`, `pipe_write` | out-of-bounds kernel read to user space; out-of-bounds kernel write |
| `net/socket.c` `do_recv` | use-after-free of the datagram, double free, and a `count` underflow ending in a NULL dereference in ring 0 |
| `net/tcp.c` tx and rx rings | same pair, on the connection buffers |
| `kernel/syscall.c` `sys_getdents64` | **the worst of them.** The loop holds raw `struct inode *` pointers across the copy and takes no reference. `detach()` calls `inode_free` the moment an unlinked inode's `refs` reaches 0, so a sibling's `unlink` during the sleep frees the entry we are iterating. We then read its `name` pointer and `name_len` out of freed memory and `memcpy` that into the user's buffer — an attacker-influenced pointer and length, read from the heap and copied out — and `vfs_dir_stop` writes a cursor into the freed node |
| `drivers/tty.c` `tty_read` | every reader of the terminal shares one ring, so the same double-consume. Beyond the disclosure, `lines[0]` underflows and `--nlines` at zero turns the `memmove` into a four-billion-byte one |
| `kernel/proc.c` `proc_wait4` | two threads both see one zombie, both reap it. `reap` clears `cwd` so the second reap is not a double free — but the slot is `PROC_FREE` by then and a `fork` can reuse it, after which the stale reap drops a **live** process's `cwd` reference (an inode refcount underflow, so a use-after-free) and marks its slot free |

### The fix, in two halves, neither sufficient alone

**`fault_in_user()`** (`arch/i386/include/uaccess.h`) touches one byte of every
page of a user range before any shared state has been read, so the copy that
follows has nothing left to fault on. Sleeping during it costs nothing because
nothing has been claimed yet. The write probe is `or byte [addr], 0` — a
read-modify-write that leaves the byte alone but still takes the *write* fault,
which is what unshares a copy-on-write page; a plain read would leave it shared
and the real copy would fault anyway. It is applied in `rw()`, the one door
`read`, `write`, `readv` and `writev` go through, and in `sys_getdents64` and
the four socket send/receive syscalls. **+34 cycles on a 4 KiB read**
(1506 -> 1540, `docs/performance.md`).

**And every site is correct across a sleep anyway**: the ring index is
committed before the copy and rolled back if the copy fails and nobody else
claimed meanwhile (`fs/pipe.c`, `net/tcp.c`, `drivers/tty.c`), or the object is
detached first and requeued if it was only a peek (`net/socket.c`), or the
zombie is reaped first and the status copied from a local (`kernel/proc.c`), or
a reference is held across the copy (`sys_getdents64`).

Why both: **`clone(CLONE_THREAD)` is supported now**, so one address space has
several threads, and a sibling can `munmap` the range and map a file over the
same address. It can only run while we are asleep, and we do not sleep after
the pre-fault returns — but the pre-fault itself sleeps, so a thread woken
while a later page is being probed can pull an earlier page out from under us.
Pre-faulting removes the sleep that happens with **no attacker present**, which
is the common case and the one that made pipe and socket reads racy. It is not
a defence against a deliberate race, and a site whose failure mode is
memory-unsafe cannot rest on it.

### Three comments that were wrong, and are now right

Worth listing separately, because the comments are what produced the bugs. Each
of these asserted the atomicity that does not hold:

- `kernel/random.c` — rewritten twice. First to claim a race that could not
  happen, then to deny one that could. It now records the wrong argument *as*
  wrong, because it is a tempting argument.
- `kernel/syscall.c` `sys_getdents64` — "Nothing can change the directory
  during this call: syscalls run with interrupts off." Now says that interrupts
  being off is one of two things that make it true, and names the other.
- `kernel/proc.c` `clone_thread` — "the new thread can't run before this
  syscall returns (interrupts are off)."

### Checked and found safe

So the list above is not read as "everywhere a copy appears":

- `fs/pcache.c` `copy_out` does `p->readers++` **before** the copy and `--`
  after, pinning the page across the sleep, and `pcache_reclaim` skips pinned
  pages. That is the correct pattern, and the file header says so. `irq_save`
  being held across the sleep is also fine: a resumed thread restores its own
  interrupt flag, and `cpu_idle` is `sti; hlt; cli`, so it hands back what it
  was given.
- `fs/ramfs.c` — `file_truncate` keeps the page table and only clears slots,
  and `page_at` re-reads `i->pages` every iteration, so a concurrent truncate
  yields zeros rather than freed memory.
- Signal delivery and `sigreturn` touch thread-local state only.
- `f->pos` in the read and write paths of ramfs, ext2 and FAT **can** still be
  double-used by two threads sharing one `struct file`. The snapshot is taken
  into a local and the end-of-file guard is applied to that local, so there is
  no underflow and no out-of-bounds access; the cost is duplicate data, which
  is a correctness wart rather than a security bug. Left as is.

### Producing the interleaving on purpose

Everything above started as an argument about an interleaving nobody had ever
made happen, which by the standard this document applies everywhere else is
not evidence. `make test` now produces it.

`include/uaccess_test.h` and `kernel/uaccess_test.c` let a thread arm the nth
`copy_to_user` or `copy_from_user` it performs to call `thread_yield()` before
copying anything. A yield is what a sleep looks like from the other side: the
sibling runs, sees whatever shared state we left behind, and acts on it. It is
a yield rather than a real disk wait so the interleaving happens every time
instead of one time in a thousand, and it is faithful because `wq_sleep` leaves
the kernel in exactly this state — inside a syscall, interrupts off. A
test-only syscall (423) does the arming. Both are inside `#ifdef SELFTEST`: a
normal build has no branch and no symbol.

`user/musl/racetest.c` uses it. Each test asserts a conservation law rather
than an implementation detail, because the law is what the bug violates and it
holds however the scheduler orders things:

- eight bytes written to a pipe can be read exactly once, in total;
- two writers of eight bytes each put sixteen distinct bytes in;
- one child is reaped by exactly one waiter, and its status survives;
- every name `getdents64` returns is a name that is really there, and an entry
  that exists for the whole walk is reported.

`make verify-racetests` puts each of the six fixes back the way it was, one at
a time, and checks that racetest notices — and names which check noticed. It is
slower than the host-side verify scripts because every case boots QEMU.

#### An eighth bug, found by the test rather than by reading

The `getdents64` fix above was wrong, and running it said so. It pinned the
entry it was copying, which is the pointer the code is obviously holding. But
`next` is fetched from `vfs_dir_first()` *before* the loop, and the copies that
emit `.` and `..` sleep before it is ever used — so a sibling frees it and the
first real iteration reads a name pointer and a length out of a recycled inode
and copies that name to user space. The test printed
`unexpected entry "\xe9b"`. Both `next` and the cursor anchor are now pinned
for as long as they are held, with one exit path that releases them.

That is the argument for this kind of test in one bug: two careful readings of
`sys_getdents64` produced a fix that was still memory-unsafe, and the first run
of the test caught it.

#### Four ways the test measured nothing, all found by verify-racetests

Every one of these passed while the bug was reintroduced, and would have been
committed as evidence:

- **the buffer was too small.** At 64 bytes only `.` and `..` fit, so the call
  returned before the loop touched an entry. 128 bytes holds `.`, `..` and
  three entries, which is enough for the stale pointer to be used in the same
  call and small enough that eight entries take several calls.
- **the sleep always landed in the same place.** One arming always lands on the
  copy that emits `.`, and two of the three `getdents64` bugs live further in.
  The test now sweeps the arming position 1..10.
- **the surviving entry was at the wrong end.** A directory lists in the
  reverse of creation order, so the first entry reported was the one kept
  alive — the pointer held across the sleep stayed valid and the missing
  reference was invisible. The survivor has to be reported *last*.
- **nothing recycled the freed memory.** Freed memory usually still holds the
  old name, so a walk reading a dangling pointer reads `"aaa"` back and sees
  nothing wrong. The sibling now grooms the heap: after deleting, it creates
  files whose names are its own choosing, so the recycled inode says something
  the walk could never legitimately return. This is what an exploit does — the
  point of a use-after-free is to choose what the freed object says — and the
  test was not an attack path until it did it too.
- **the deletion order let the bug get away with it.** `sibling_remove` leaves
  the removed node's own `next_sibling` alone, so a detached node still points
  at whatever followed it. Deleting in creation order walks the list from tail
  to head, and by the time each node is removed its `next_sibling` already
  points at the survivor — every detached node ends up pointing at a *live*
  entry. Deleting newest-first makes them point at freed memory, which is the
  case the fix is for. An attacker picks the order; the test should pick the
  bad one.

### What is still missing

`drivers/tty.c` has no attack-path test. Reading `/dev/console` from a test
would take the shell's input away from it, so the `tty_read` fix is still only
an argument. The shape is identical to the pipe's, which is tested, but that is
an analogy and not a test.

## Two bugs in `execve`'s argument copying

Found by a review of the ELF loader and signal paths, not by a test, and both
in code that had been read several times. Both are in `copy_strings`
(`kernel/proc.c`), which copies a user's `argv` or `envp` into a kernel buffer.

**argv was allowed to fill the array that argv and envp share.** `struct
exec_args` has one `char *argv[EXEC_MAX_ARGS + 1]` array; `envp` is placed just
after argv's terminating NULL, and its room worked out by subtraction. With
`argc == EXEC_MAX_ARGS` (1024, which was allowed), `envp` landed one slot past
the end of the array — the address of the `envp` field itself — and its room
came out as `-1`. The loop's only stopping condition is `n == room`, and `n` is
never negative, so the envp loop had no bound at all. It wrote a pointer over
`envp`, then over `used`, then copied user strings to wherever the corrupted
`used` pointed: a kernel-mode store with no exception-table entry. Reachable
from an ordinary `execve` with 1024 arguments and two environment variables.

The bound is now `EXEC_MAX_ARGS - 1`, which is what "argc + envc + 2 pointers
must fit" actually implies, and `copy_strings` refuses a negative room rather
than trusting its caller's arithmetic. Either change alone closes it; both are
there because the arithmetic was wrong in two places and fixing only one leaves
the next reader to rediscover why it is safe.

**A NULL argv or envp was not terminated.** A NULL vector is legal and means
"empty", and the early return for it forgot to write the terminating NULL —
leaving `out[0]` holding whatever `kmalloc` last had at that address. `count()`
in `kernel/usermode.c` then walks that as a vector of `char *` until it finds a
zero word, and `memcpy`s each stale word's contents onto the new program's
stack. Kernel heap handed to user space as `argv`, or a fault in kernel mode.

### Heap poisoning, because the test for the second one passed

The first bug's test works immediately: 1024 arguments, and with the bug
reintroduced the kernel page-faults and `make test` fails. The second one was
the interesting one. Three successive attempts at a test for it passed with the
bug in place:

- `execve("/bin/true", 0, 0)` — survived, because `/bin/true` ignores `argv`.
  The kernel copying stale heap into it changes nothing observable.
- `execve("/bin/args", 0, 0)`, which exits with its own `argc` — still passed,
  because the recycled `exec_args` block read back as zeroes, so `count()`
  stopped at the first word and found `argc == 0`, which is the right answer.

The second failure is the general case, and it is not specific to `execve`:
**zero-filled memory hides every uninitialised-memory bug.** An uninitialised
pointer reads as NULL, every loop looking for a terminator stops at once, and
the wrong code produces the right answer. There is no sanitiser on the kernel
itself to catch this — the fuzz targets run on the host, this runs on the
metal.

So `make test` builds now fill every `kmalloc` with `0xA5` (`mm/kheap.c`,
`#ifdef SELFTEST`). `0xA5A5A5A5` is above `KERNEL_VBASE` but unmapped, so
dereferencing one faults instead of quietly working, and it is not a NUL, so no
terminator search stops early. With poisoning on, the second bug is caught:
`count()` walks into the poison and the kernel faults. Normal builds do not
touch the memory `kmalloc` returns, so nothing on a hot path pays for this.

Worth noting that turning poisoning on broke nothing: 132 self-tests still
pass, which is evidence that nothing in the kernel was relying on `kmalloc`
returning zeroed memory. That is the other thing this flushes out, and it found
nothing, which is the result you want.

## DHCP, and the two holes it had to open

`net/dhcp.c` exists because UTM's `Shared` mode (Apple's vmnet) hands out
addresses by DHCP and has no fixed ones to hard-code, so without a client
there was no working network on a real machine at all. The address, netmask,
gateway and nameserver moved from compile-time `#define`s to four words in
`net/ipv4.c`, written once by the client before any socket exists.

DHCP is a protocol for taking configuration from a stranger, so the threat
model is the server. Nothing it says is trusted beyond being a plausible
answer:

- **The transaction id comes from the CSPRNG.** A guessable one lets anyone on
  the segment answer a request they never saw, and matching the xid is the
  whole of DHCP's (weak) defence against that.
- **The address offered is checked**: zero, 255.255.255.255, multicast
  (224/4) and loopback (127/8) are refused, because accepting any of them
  would make every subsequent send invalid or send our own traffic back to us.
  A server offering one is broken or hostile; either way we keep waiting.
- **The ACK must acknowledge the address we asked for**, not a different one.
- **Option parsing walks the bytes we actually have.** An option header needs
  two bytes and its body needs its length byte more; anything that does not
  fit ends the walk rather than being clamped, because a truncated option
  means the packet is not what it claims and the rest cannot be trusted
  either. This is a fuzz target (`make fuzz TARGET=dhcp`), and the bounds
  check was verified by deleting it and watching the fuzzer find the overread
  — which it only could after the harness stopped over-allocating the packet
  buffer (see below).
- **A netmask of zero is replaced** with the class-C guess, because a zero
  netmask puts every address off-subnet.

Two things had to be opened up, and both are as narrow as they can be made.

**Sending from 0.0.0.0 to 255.255.255.255.** Every other path refuses to send
a broadcast, because broadcast is how a machine becomes an amplifier. A
machine with no address has no other way to ask for one, so the exception is
one function (`ip_send_bootp`) with one caller. `ip_send` itself now refuses
to send anything at all while `ipv4_addr` is zero.

**Receiving a datagram addressed to 255.255.255.255.** The receive path drops
anything not addressed to us, for the same amplifier reason — and a bound UDP
port cannot tell a broadcast datagram from a unicast one, so it cannot be left
to make that distinction itself. A DHCP server has no way to reach a machine
with no address, so its reply comes to the broadcast address. The exception is
gated on four things at once: a transaction must be open (`dhcp_open()`, which
is true for a few seconds at boot and never again), the protocol must be UDP,
the datagram must still reach a bound port, and the only port bound is 68. A
broadcast ping during those seconds is dropped like any other, because only
UDP gets that far.

### The harness was blind again, the same way

The DHCP fuzz target did not notice a deliberately deleted bounds check. The
reason was in the harness, not the target: `pk_feed` did `alloc_skb(n + 64)`
with a 16-byte reserve, leaving 48 bytes of slack behind the packet. An option
claiming a body longer than the packet made the parser read four bytes past the
end — and all four were still inside the allocation, so ASan had nothing to
report. Now `alloc_skb(n + 16)`, exactly the headroom, so the packet ends where
the allocation ends.

This is the third time this exact mistake has cost a day: the ext2 target gave
1 KiB blocks a whole page each, and then this. **A buffer must exactly fill its
allocation or the sanitiser cannot see a small overread**, and the only way to
know it can is to break the code on purpose and check that the harness
notices.

### Not done

No lease renewal: the lease time is parsed but ignored, and the address is kept
forever. On a network whose server reassigns it, this machine would keep using
an address that belongs to someone else. Renewal also means the four words
could change under a live socket, which nothing is currently written to expect
— so it needs the sockets to be told, not just a timer.

## Plan

In rough dependency order. Each step should end with something testable, and anything on a
hot path needs a before/after from `make bench` per the performance rules.

- [x] **A real CSPRNG first.** Everything else depended on it. ChaCha20 with fast key
      erasure, keyed from boot-time TSC jitter measured against ISA port cycles — the P4 and
      Pentium M have neither RDRAND nor RDSEED, so it has to be measured. `getrandom` and
      `AT_RANDOM` now come from it. See "The CSPRNG" above, including what the entropy is
      honestly worth.
- [x] **The memory-safety, liveness and reflection bugs above** (see "Fixed").
- [x] **Keyed ISN (RFC 6528)**, random ephemeral ports (RFC 6056), random DNS transaction
      ids, and per-destination IP IDs. All four were waiting on the CSPRNG and nothing else.
- [x] **Per-peer rate limits with jitter** on every packet we generate in response to a
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
