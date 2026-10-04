# Performance

MyOS is built for **speed and predictability on old hardware** (Pentium 4 / Pentium M);
see the performance rules in `CLAUDE.md`. This file holds the plan, what's done, and the
measured results. Every speed change needs a before/after number from `make bench`.

**Where we can beat Linux:** syscall and interrupt cost, context-switch cost, worst-case
latency and slot jitter, boot time, memory footprint. Linux will stay ahead on throughput of
big, complex workloads — we don't add complexity chasing that.

## How we measure

`make bench` builds a kernel with benchmarks (`kernel/bench.c`, `user/bench.c`), boots it
headless and prints average cycles per operation.

- QEMU runs with **`-icount shift=0,sleep=off`**: time is derived from the number of
  instructions executed, so results are exactly repeatable — two runs give identical
  numbers. In this mode a "cycle" is roughly **one instruction**. That's right for comparing
  versions of our code, but it is **not** real Pentium 4 time: cache misses, slow
  instructions (`int 0x80`, port I/O, `popf`) and the serial port's real speed are invisible.
- QEMU runs with **`-cpu pentium3`**: the feature set both target CPUs have, so we never use
  something the old machines lack. `cpu_init()` checks the features at boot.
- Real numbers: run `make bench` on the real machine (step 12). For a Linux comparison on the
  same machine, use lmbench's `lat_syscall`, `lat_ctx`, and boot time.

## Results

QEMU, instruction-count mode (lower is better):

| benchmark                         | baseline | now    | change                     |
|-----------------------------------|---------:|-------:|----------------------------|
| boot: kernel ready, 64 MiB (µs)   |   11 608 |  1 111 | CPU work; 119 before BusyBox + bash (~330 initramfs entries, ~1.7 µs each); 9b: +36 finding disks, mounting /data; +18 for the CSPRNG's boot entropy (1 093 without it) |
| boot: bash prompt, 64 MiB (µs)    |        — |  2 024 | CPU work. Was 6 907 (kernel + 6 144 for /init and bash). 9c-2 maps the files and faults pages in, so that part is 1 204 µs |
| boot: kernel ready, 512 MiB (µs)  |        — |    181 | was 4 181 before the bitmap fix (23×) |
| memcpy 4 KiB                      |   12 315 |  1 053 | 11.7× (`rep movsd`)        |
| memset 4 KiB                      |   10 266 |  1 049 | 9.8× (`rep stosd`)         |
| kmalloc+kfree 64 B                |      109 |     67 | 1.6× (size classes); +4 for the double-free mark (step 9) |
| … with 1000 objects live          |   15 105 |     63 | 240×, and now constant     |
| thread switch                     |      188 |     94 | 2× (run queue, step 8)     |
| exec + exit (ELF `true`)          |  218 177 | 33 779 | 6.5× (memset, demand paging); 9c-2: +148, the entry page is faulted instead of a whole-file copy (a tiny file, so no win here) |
| null syscall, `int 0x80`          |      115 |    128 | +11: registers saved for fork, signal check |
| null syscall, `sysenter`          |        — |    158 | see note                   |
| fork + exit + wait                |        — | 51 403 | step 8 (copy-on-write); 8i: +187, cwd and mapping refs |
| fork + exec + exit + wait         |        — | 84 316 | step 8; 8i: +128           |
| slot start jitter, worst (µs)     |        4 |      4 | —                          |
| disk read 64 KiB, CPU work        |  223 722 |  1 250 | 9d: bus-master DMA, one command and one IRQ (PIO was 41 788; see "What instruction counts hide") |
| block-cache hit (bread + brelse)  |        — |    102 | step 9: hash lookup, LRU move |
| open + close (`/test/numbers.txt`) |       — |  1 441 | step 7: 1 131; +23% symlinks, per-process fds; 8i: +46 (inode refs, O_CREAT/O_TRUNC checks) |
| lseek + read 4 KiB (initramfs)    |        — |  1 509 | step 7: 1 479; 8i: +30 (access-mode check) |
| stat (`statx`)                    |        — |  1 271 | step 7: 1 044; 8i: +32 (three separate times) |

**Boot picture:** stage 2 shows the logo and holds it for 2 s (so it can be seen in QEMU,
where loading takes a blink): boot to a prompt is ~2.5 s with it. Removing `call hold` in
`arch/i386/boot/stage2.asm` gives the 0.5 s below back.

**Boot to a prompt, wall clock:** in QEMU on the Apple Silicon Mac, from starting QEMU to
bash's prompt took ~0.5 s before the boot picture's hold: ~0.36 s is QEMU itself, SeaBIOS and our loader copying the 3.1
MB initramfs; ~0.13 s is the kernel, /init and bash, emulated. On a real Pentium 4 the
6.8 M instructions are a few ms; what dominates there is the BIOS (POST, 5-10 s, not ours)
and the loader reading kernel + initramfs (3.2 MB) through the BIOS: ~0.1-0.3 s from an
IDE disk, but 1-5 s from a USB stick on a BIOS with slow USB emulation. Before BusyBox and
bash the initramfs was ~0.2 MB, so this didn't matter. Options when it does (measure on
the real machine first): a compressed initramfs (half the bytes to read, ~0.1 s to unpack
on a P4), or a small initramfs with the rest read by our own DMA disk driver (step 9).

**Note on boot numbers:** since step 9 the boot reads the disks (partition tables, the
ext2 superblock and root directory). `make bench` shows that waiting apart from the CPU's
work, for the same reason as below; on a real hard disk it's a few seeks (~10 ms each).

**Boot from the disk (9c), QEMU wall clock, root mounted -> bash prompt:** 3.1 s with a
1 MiB block cache; 2.1 s with an eighth of RAM; **0.3 s** with a short spin (2048 status
reads) before sleeping on the disk's interrupt. QEMU needs ~2.5 ms to wake a halted
virtual CPU, so every interrupt wait cost that; data is ready within microseconds there.
On a real disk the spin costs up to ~2 ms of CPU per seek before sleeping — measure on
hardware; DMA (9d: one interrupt per 64 KiB) makes it mostly moot. CPU work to a prompt
(make bench, instruction count), before the page cache: 3.3 ms from RAM vs 18.7 ms from
the disk, because exec copied whole files. After 9c-2, on a RAM root, /init + bash is
1 204 µs of CPU (the table's bash-prompt row). The disk-root number has not been
re-measured; bash's pages still come off the disk on first touch, one page at a time,
with the next page read ahead.

**Note on disk numbers:** QEMU's "disk" answers at once in host time, but while our CPU
sleeps waiting for it, instruction-count mode jumps the virtual clock ahead by however long
the Mac took — elapsed times vary 10× between identical runs. So `make bench` counts only
the CPU's work (elapsed minus idle time, which the kernel now tracks). How fast a real disk
is needs real hardware.

**Note on `sysenter`:** in instruction-count mode it looks *slower*, because it executes a few
more (cheap) instructions, while `int 0x80` is a single instruction that QEMU charges as one.
On a real Pentium 4, `int 0x80` alone costs well over a thousand cycles — the reason Linux
switched to `sysenter` in 2002. Confirm on real hardware.

**Note on jitter:** with QEMU in normal (real-time) mode the worst slot start was ~5 ms
late; that was the host delivering timer ticks in bursts, not MyOS. In virtual time it's 4 µs.

### What instruction counts hide (2026-10-03)

`-icount` charges one "cycle" per instruction, whatever the instruction costs on real
hardware. Three costs were invisible that way:

- **PIO disk reads.** `rep insw` of 64 KiB is 32 768 reads of the IDE data port. That
  counted as ~41 800 instructions; on real hardware each read is an I/O-bus cycle
  (PIO mode 4: >= 120 ns per word), so 64 KiB keeps the CPU busy for ~4 ms, about
  12 million cycles on a 3 GHz Pentium 4. Linux uses bus-master DMA, where the CPU
  does a dozen port writes and takes one interrupt. Fixed: `drivers/ata.c` now uses
  DMA too (1 250 instructions per 64 KiB, ~13 port I/Os per command), PIO only as a
  fallback.
- **`wbinvd` in the e1000 driver.** It wrote back and emptied the whole CPU cache on
  every transmit and two or three times per receive poll: one instruction to QEMU,
  tens of microseconds to ~1 ms on a real P4, plus every later cache miss. PC DMA is
  cache-coherent; the driver now uses a compiler barrier (what Linux's dma_wmb() is
  on x86). The network table below does not change, because QEMU never saw the cost.
- **Spinning on the disk status port** before sleeping (2 048 reads, ~1 us each on
  real hardware). Added for QEMU's slow wake-ups; the DMA path does not spin.

Rule from this: a path's instruction count is only half the story. Port I/O, MMIO
reads, cache flushes and serializing instructions must be counted separately and
kept out of hot paths; real-hardware `rdtsc` numbers settle it.

### What the CSPRNG cost (2026-10-03)

Four network values that had to stop being guessable (`docs/security.md`) all needed one
generator, so the question was what it costs.

| path | before | after | note |
|---|---:|---:|---|
| boot: kernel ready, 64 MiB (µs) | 1 093 | 1 111 | +18 µs, measured by removing the `random_init()` call and re-running |
| slot start jitter, worst (µs) | 4 | 4 | unchanged — the thing that matters most |
| null syscall, `int 0x80` (cycles) | 130 | 130 | unchanged |
| null syscall, `sysenter` (cycles) | 158 | 158 | unchanged |
| kernel size (bytes) | 160 404 | 162 512 | +2 108 for ChaCha20, the generator and the rate limiter |

**The boot number is the one QEMU lies about**, in exactly the way the section above warns.
The entropy comes from timing 512 reads of port 0x80, and `-icount` charges one instruction
for a port read that costs about a microsecond on a real Pentium 4. So expect nearer 0.5 ms
on the target hardware rather than 18 µs — under 2% of a boot that already spends 27 ms
waiting for disks, and it buys the key that every other secret in the stack hangs off. It is
also the reason the sample count is 512 and not 5 000.

Nothing else moved, by design. Every caller of the generator wants a few bytes once per
connection or once per lookup, so a ChaCha20 block (20 rounds of 32-bit adds, xors and
rotates, no tables, no SSE) never lands on a hot path. The two values that *are* per-packet
were written to avoid it: the IP ID is a counter in a bucket chosen by one multiply and a
shift, and the rate limiter's jitter draws 32 bits once and spends them eight at a time.
Slot jitter staying at 4 µs is the number that proves none of it reached the scheduler.

### Network happy path: MyOS vs Linux (2026-10-03)

`user/netbench.c` is one static binary. MyOS runs it from `make bench` (after the
slot scheduler starts, so the network thread runs as in a normal boot); Linux runs it
as `/init` (`tools/linux-netbench.sh`: Debian 12's stock `linux-image-6.1.0-50-686`,
6.1.176, e1000 loaded as a module). Same QEMU: `-cpu pentium3 -m 64M`, e1000, user
net, `-icount shift=0,sleep=off`. `tools/echo-server.py` on the host echoes. 64-byte
messages; UDP on a connected socket (1 600 echoes), TCP with `TCP_NODELAY` (800
echoes, one at a time). Instructions per echo (lower is better):

| path                                 | Linux 6.1 | Linux `mitigations=off` | MyOS  |
|--------------------------------------|----------:|------------------------:|------:|
| null syscall (`getpid`, `sysenter`)  |       365 |                     364 |   169 |
| UDP `write()`                        |     6 291 |                   6 284 | 1 715 |
| UDP `read()` (already queued)        |     2 004 |                   1 985 |   426 |
| UDP kernel work outside syscalls     |     3 748 |                   3 843 | 1 433 |
| **UDP total per echo**               |**12 044** |              **12 112** |**3 574** |
| TCP `write()`                        |    10 609 |                       — | 1 782 |
| TCP `read()`                         |     2 406 |                       — |   423 |
| TCP kernel work outside syscalls     |     7 845 |                       — | 3 275 |
| **TCP total per echo**               |**20 861** |                       — |**5 480** |

Totals are the fair rows: both kernels do receive work outside the syscalls (Linux in
the softirq, MyOS in its network thread, which costs a thread switch each way). Run
to run, Linux's "outside" rows vary up to ~8% (when the echoes land); MyOS's UDP
total was 3 452-3 574 across changes. MyOS also computes the IP, UDP and TCP
checksums in software, where Linux has the e1000 do them.

What this does not show: real cycles. Instruction count hides cache misses, the
pipeline cost of retpolines, MMIO and port I/O, all of which matter on a Pentium 4.
`mitigations=off` changes Linux by under 1% here *in instructions*; on a real P4 the
retpolines cost more. The real comparison needs `rdtsc` on the target machine.

Why Linux is longer, by inspection (not profiled): socket and memory accounting,
LSM hooks (Debian runs AppArmor and Landlock), cgroup and netfilter hooks, the qdisc
layer, a general route lookup, skb clone/refcount work, per-call iov_iter setup, and
for TCP: congestion control, RTT estimation, timestamps/SACK options, delayed-ACK and
pacing timers. MyOS skips all of these. Some are security or robustness features
(`docs/gaps.md`, network security; TCP has no congestion control yet).

### Disk happy path: MyOS vs Linux, four disk types (2026-10-03)

`user/diskbench.c` is one static binary; `tools/diskbench.sh TYPE` runs it on MyOS
(from `make bench`) and on Linux (`tools/linux-diskbench.sh`: the same Debian 6.1
kernel as `/init`, drivers loaded as modules), each on a fresh copy of the same
64 MiB ext2 image (1 KiB blocks, two 8 MiB files of random data). Same QEMU:
`-cpu pentium3 -m 64M -icount shift=0,sleep=off`. A spinner child counts the CPU
time the benchmark's own calls took away from it, so this is CPU work per call,
including interrupts and threads, not waiting for the disk. Instructions per call
(lower is better):

| operation                     | type   | Linux 6.1 | MyOS before | 9d (bcache writes) | MyOS now | Linux / MyOS |
|-------------------------------|--------|----------:|------------:|---------:|---------:|-----:|
| cold read 64 KiB, in order    | IDE    |   124 784 |     126 238 |   28 556 |   27 618 | 4.5× |
|                               | AHCI   |    77 015 |           — |   28 494 |   27 174 | 2.8× |
|                               | NVMe   |    97 977 |           — |   28 425 |   27 883 | 3.5× |
|                               | virtio |    91 602 |           — |   27 969 |   27 505 | 3.3× |
| warm read 64 KiB, in order    | all    |   ~37 030 |     125 765 |   18 192 |   18 144 | 2.0× |
| warm read 4 KiB, random       | all    |    ~3 528 |       8 490 |    1 626 |    1 623 | 2.2× |
| cold read 4 KiB, random       | IDE    |    29 678 |       8 662 |    6 335 |    4 204 | 7.1× |
|                               | AHCI   |    17 782 |           — |    6 349 |    4 205 | 4.2× |
|                               | NVMe   |    16 483 |           — |    6 439 |    4 350 | 3.8× |
|                               | virtio |    18 257 |           — |    6 287 |    4 128 | 4.4× |
| write 64 KiB (+ one fsync)    | IDE    |   711 578 |     317 091 |  133 961 |  100 977 | 7.0× |
|                               | AHCI   |   734 799 |           — |  130 077 |  100 538 | 7.3× |
|                               | NVMe   |   486 119 |           — |  150 174 |  101 006 | 4.8× |
|                               | virtio |   478 792 |           — |  129 721 |  100 707 | 4.8× |

"Before" is IDE only (the other drivers didn't exist). What changed, in order, with
the IDE number after each step:

1. **`read()` through the page cache** (`fs/pcache.c`, rewritten): 4-way
   set-associative, a quarter of RAM; a cached page costs one lookup and one copy, no
   file-system lock. Before, every 1 KiB block went through `bmap` and the block
   cache, which holds only 2 MiB of 1 KiB blocks, so an 8 MiB file was never "warm".
   Warm 64 KiB 125 765 → 18 192; warm 4 KiB 8 490 → 1 626.
2. **Read-ahead with scatter-gather DMA** (`ext2_readpages`, `blockdev_readv`): a miss
   maps a run of pages (4, growing to 32 while the file is read in order) and reads
   each disk run with ONE command straight into the cache's frames. Cold 64 KiB
   126 238 → 48 461.
3. **One indirect-block lookup per 64 blocks** (`bmap_run`) instead of one or two per
   block. Cold 64 KiB 48 461 → 28 556.
4. **Write clustering** (`bcache.c` `write_run`): dirty neighbours go out as one
   command, both at `sync` and when the cache evicts; plus an O(1) "next block" hint
   in `alloc_block` instead of a bitmap scan from the start (files also end up
   contiguous). Write 321 700 → 182 395 → 133 961.

5. **Writes into the page cache** (`ext2_write`, `pcache_get_write`): a write copies
   into the file's page and marks it dirty; a page replaced whole is never read
   first. Write-back sends runs of up to 32 dirty pages as one scatter-gather command
   (`ext2_writepages`). New data blocks skip the block cache (`bforget`) — they were
   zeroed there, written, then overwritten. Whole pages need no NVMe bounce copies.
   Write 133 961 → 100 977 (NVMe 150 174 → 101 006). Cold random 4 KiB 6 335 →
   4 204: the read path no longer asks the block cache about every block (`bpeek`).
6. **A 16-byte cache slot** (the owner of a dirty page moved to a side table only
   write-back touches): one set of 4 slots is one 64-byte cache line, and a slot
   index is a shift, not a multiply. Warm 64 KiB 18 736 → 18 144.

Warm 64 KiB is now within 1 800 instructions of the 64 KiB copy to the user buffer
itself (`rep movsd`: 16 384). Going lower needs zero-copy (`mmap`, which already
maps cache pages).

The same benchmark with less RAM (`MEM=20M`, `MEM=12M tools/diskbench.sh virtio
myos`) checks that the cache gives memory back: at 20 MiB the numbers are within 3%
(write 104 227); at 12 MiB an 8 MiB file no longer fits, so "warm" reads are partly
misses (warm 64 KiB 25 717, 4 KiB 3 209) — and nothing runs out of memory.

### Read-ahead that doesn't wait (2026-10-03, step 9d-3 part 1)

`blockdev_submit` starts a disk request and returns; the page cache uses it to read
the next read-ahead window while the program copies the current one (`fs/pcache.c`,
P_RA / P_IO). `make test` shows it working: of the 71 pages of `numbers.txt` read in
order, 68 arrive this way.

**What QEMU can't show.** The point is that the disk and the CPU work at the same
time. QEMU's "disk" is the Mac's RAM (~400–550 MB/s), so there is almost no waiting
to hide. Its disk throttle (`BPS=` in `tools/diskbench.sh`) is a token bucket that
saves up credit while the guest computes, so even the old synchronous code looks
overlapped (both ~110 ms per 8 MiB at 60 MiB/s). `tools/diskbench.sh` with
`ICOUNT=0` runs `user/streambench.c` on wall-clock time: no measurable difference
in either direction (IDE 21–25 ms before vs 17–28 ms after, virtio 36–37 vs
34–35 ms, 8 MiB read + checksum; host noise is larger than the change).

**What it should do on the old PC** (estimate, not measured): reading 8 MiB at an
old disk's ~60 MB/s is ~140 ms of disk time. A program that does ~4 instructions
per byte (`cksum`) adds ~15 ms of CPU, so waiting for each window costs ~10%. A
program like `gzip` (~50–100 instructions a byte) adds ~200–400 ms of CPU. Waiting
for each window then costs the whole disk time on top: ~350–550 ms, instead of the
larger of the two (~200–400 ms) when they overlap. That is up to ~40% less time.
The real number needs the old PC.

**What it costs** (instructions per call, `diskbench`, all four types): cold 64 KiB
~27 600 → ~28 500 (+3%: building the requests, and a thread switch to the disk's
worker); warm 64 KiB 18 144 → 18 293 (+1%, the "is this page still being read?"
check on every hit); cold random 4 KiB ~4 200 → ~4 300. Writes unchanged.

The IDE driver's short spin before sleeping (`POLL_FIRST`, for QEMU's slow wakeup)
now happens only when no other thread could use the CPU; before, it would have
spun in the worker while the program waited to copy.

### Cache sizing: why all free RAM, with numbers (2026-10-03)

**Cost of a hit and of a miss.** A hit is the 1 623 instructions above (lookup + 4 KiB
copy): on a 2.4 GHz Pentium 4 about 1–2 µs, plus the copy's memory traffic (DDR-era
memory copies ~1–2 GB/s, so ~2–4 µs), call it **3 µs**. A miss on the old PC's disk is
a seek plus half a rotation: a 7200 rpm IDE disk ~8.5 ms + 4.2 ms ≈ **12.7 ms**; a
5400 rpm laptop disk (Pentium M) ~12 ms + 5.6 ms ≈ 17.6 ms. A SATA SSD: ~0.1 ms.
(Datasheet-typical figures, not measured here.) So:

| disk          | miss       | hit   | a miss costs |
|---------------|-----------:|------:|-------------:|
| 7200 rpm IDE  | 12 700 µs  | 3 µs  | ~4 000 hits  |
| 5400 rpm      | 17 600 µs  | 3 µs  | ~6 000 hits  |
| SATA SSD      |    100 µs  | 3 µs  |    ~30 hits  |

**Average access time** is `T = h·t_hit + (1−h)·t_miss`. On the IDE disk:

| hit rate h | average T | vs. all hits |
|-----------:|----------:|-------------:|
| 90%        | 1 273 µs  | 424× slower  |
| 99%        |   130 µs  |  43× slower  |
| 99.9%      |    16 µs  |   5× slower  |
| 99.99%     |     4 µs  | 1.4× slower  |

Each extra "9" of hit rate makes the average 10× faster, while the hit path itself
(what the CPU work above optimizes) only matters past 99.9%. **The hit rate is worth
more than any instruction count**, and the hit rate is set by one thing: whether the
working set fits in the cache. So the cache gets every frame no one else wants:

- It may hold all free RAM; the page allocator calls `pcache_reclaim` when it runs
  out, which gives back clean pages (clock order, bounded: 2 rounds × 1 024 pages).
  Programs always win over cached data; idle RAM is just wasted.
- The table has a slot for every frame (next power of two, at most 2^18 = 1 GiB):
  16 bytes + 4 bytes of owner = **20 bytes per 4 KiB, 0.5% of RAM** (320 KiB at
  64 MiB). The frames, not the table, are the limit.
- 4 ways per set: a page can only go in 4 slots, so two hot pages can push each
  other out early. With slots ≥ frames and a file's pages in consecutive sets, a file
  only collides with itself when it is bigger than the cache; between files the
  chance that 5 hot pages share one set is small when the cache is not full.

**Working sets on the old PC** (estimates, to size against): a shell session with
busybox and a few tools is ~2–5 MiB; compiling a small C program with gcc touches
`cc1` (~20 MiB), `as`, `ld`, libc headers (~5–10 MiB): ~30–40 MiB; a P4 had
256 MiB–1 GiB of RAM. So everyday work fits in RAM with room to spare — after the
first touch the disk is only needed for writes — if, and only if, the cache can grow
into free RAM. The old fixed quarter of RAM (16 MiB at 64 MiB) did not fit a compile.

**Dirty pages: RAM/8, at most 32 MiB.** Dirty pages can't be dropped, only written;
a `sync` or `fsync` waits for them. At an old disk's ~50 MB/s sequential, 32 MiB is
**~0.6 s** — the longest a `sync` can wait on queued writes, whatever the RAM size
(RAM/8 alone would be 2.6 s at 1 GiB). The background writer runs every second.

**Read-ahead: 4 pages, doubling to 32 (128 KiB).** One seek (12.7 ms) buys 128 KiB
(2.1 ms at 60 MB/s): only ~14% of the disk's streaming speed if every window costs a
seek. In order it doesn't — the next window is the next sectors and the drive's own
buffer has read them — but the reader still waits for each window before the next is
asked for (`docs/gaps.md`, one command at a time). Reading the next window *while*
the program copies this one is the next step.

What this does not show: the disk. QEMU's disks answer at once; a real disk's seek
(ms) and an SSD's latency are the same for both kernels. It also hides the cost of
port I/O and MMIO (IDE's ~13 port accesses per command; AHCI/NVMe/virtio need 1-4),
cache misses and `rep movsd` speed on a P4. Real numbers need `rdtsc` on the target.

## Done

1. **Measurement**: cycle counter (`rdtsc`, `arch/i386/include/cycles.h`) calibrated in the
   background against the PIT; CPU feature detection (`arch/i386/cpu.c`); `make bench`.
2. **Asynchronous console**: serial output goes into an 8 KiB ring drained by IRQ 4
   (`drivers/serial.c`); `kprintf` hands over whole pieces of text, so the VGA cursor (4 slow
   port writes) moves once per piece, not per character. `write()` never waits for the
   serial port — when the queue is full it does a short write. Crashes flush by polling.
3. **`memcpy` / `memset` / `memmove`** with `rep movsd` / `rep stosd` (`arch/i386/memcpy.c`).
4. **No test code in a normal boot**: three build modes (normal / `make test` / `make bench`).
   The normal kernel is 25 KB; the test kernel 45 KB.
5. **O(1) slot lookup**: the scheduler keeps a slot → thread table, rebuilt only when threads
   come or go (`kernel/sched.c`).
6. **4 MiB pages (PSE)** for the kernel's direct map, and **global pages (PGE)** for all
   kernel mappings — they'll survive CR3 loads once each process has its own page directory.
7. **`sysenter` + vsyscall page** (`arch/i386/sysentry.asm`, `sysenter.c`): `AT_SYSINFO` in
   the auxiliary vector, so musl and glibc use it automatically. `int 0x80` still works.
8. **User memory via exception fixups** (`arch/i386/uaccess.asm`, `uaccess.h`): range check +
   copy; a fault jumps to a fixup that returns `-EFAULT`. No page-table walks. Also turned on
   **CR0.WP**, so the kernel can't be tricked into writing a program's read-only pages.
9. **`kmalloc` size classes** (`mm/kheap.c`): O(1) for objects up to 1 KiB; big blocks merge
   with their neighbours in O(1).
10. **FPU/SSE state** saved eagerly for user threads only; kernel built with
    `-mgeneral-regs-only`; no lazy FPU (insecure).
11. `-march=i686` for the kernel and test programs.
13. **Page allocator setup** marks whole 32-frame bitmap words at once (all-used words
    counted instantly), so boot hardly depends on RAM size any more.
12. **Step 7, zero-copy initramfs**: the archive is unpacked once into an inode tree whose
    names and file data point into the archive; path lookup is one hash probe per
    component; `read()` copies straight from the archive into the user buffer; programs run
    from `/bin` with the ELF loader reading the archive in place.

14. **Step 8, processes the fast way**: copy-on-write `fork` (page tables copied, no
    memory), demand paging for stack/heap/mmap, a page directory per process (kernel
    mappings global, no CR3 load when switching to kernel threads), a run queue so
    picking the next thread is O(1), sorted timer list (O(1) per tick), signal check
    inlined (one compare per syscall when nothing is pending). Thread switch 188 -> 94.

## Designed in, built with its step (binding — see `CLAUDE.md`)

- **Measure on hardware**: a thread woken by a disk interrupt while the CPU idles now
  runs at once instead of at the next 1 ms tick (up to 8 ticks per 64 KiB read before).
  QEMU's instruction-count clock can't show it (idle time jumps); a real IDE disk will.
- **Revisit**: unpack the initramfs lazily (a directory's entries on first lookup) —
  boot grew from 119 to 713 µs with ~330 files (BusyBox links, bash, libraries).
- **Revisit**: a dynamically linked program's libraries are copied page by page from
  the initramfs on first touch; a page cache shared between processes (step 11) would
  map the same pages into every bash.
- **Revisit**: path lookup got 20% slower in step 8 (symlinks, per-process cwd/fds):
  e.g. skip the `strlen`, cache the last lookup.
- **Step 9**: disk I/O with IRQs (done) + bus-master DMA, read-ahead, a page cache.
- **Step 11**: one page cache for `read()` and `mmap()` (done for reads, 9d); writes
  through the page cache too; a real vDSO with `clock_gettime`.
- **APIC timer**: one-shot mode, skip ticks nobody needs (power).
- **Preemptible syscalls** (trap gate) once the kernel has locks; until then every syscall
  must stay short (rule 2).

## Not worth it (or be careful)

- Tuning for QEMU — its timings aren't real.
- `-O3`, hand-written assembly beyond the string functions, clever data structures — only
  when a benchmark says so.
- Lazy FPU switching — insecure (LazyFP, 2018).
- User-space drivers cost extra context switches per I/O: decide per driver, keep the hottest
  in the kernel.

## What the kernel-wide audit fixes cost (2026-10-03)

None of this round was a speed change, but the rules say measure, so:

| | recorded before | after |
|---|---|---|
| null syscall, `int 0x80` (cycles) | 130 | 136 |
| null syscall, `sysenter` (cycles) | 158 | 162 |
| thread switch (cycles) | 97 | 97 |
| `open` + `close`, 2-part path (cycles) | 1 441 | 1 472 |
| boot, CPU work (us) | 1 111 | 1 121 |

**Read the syscall rows with care: they are not mine to claim or disown.**
Another agent was changing `kernel/signal.c`, adding `kernel/futex.c` and
touching the block layer in the same tree while this ran, and the syscall entry
path runs the signal check. Nothing I changed is on the null-syscall path at
all, so the +6 and +4 are either theirs or noise, and I have not separated
them.

What the fixes actually add, in places the benchmarks do not cover:

- **Receive path:** `ip_source_ok` is four compares and a mask per arriving
  packet, on a path that already does a header parse and a checksum. Below the
  noise floor of anything measurable here.
- **Directory walks:** `dirent_ok` adds one compare per entry (`name_len + 8`
  against `rec_len`) to the three ext2 entry loops. `open + close` moved
  1 441 → 1 472 cycles, +2%, which is the closest thing to a real reading of
  it — though that path also goes through the block layer the other agent was
  editing.
- **Pipe, TCP and socket reads and writes:** no added work. The commit of
  `head`/`tail`/`len` moved from after the copy to before it, and the TCP
  rings' `min` now has one more argument. The rollback on `-EFAULT` is on an
  error path.
- **ext2 mount:** six more compares, once per mount.
- **`rt_sigreturn`:** `gdt_user_seg_ok` is two loads and three mask tests, per
  signal return rather than per syscall.

The numbers that matter most for this project are unchanged: thread switch 97
cycles, and the slot scheduler still places every task in its own slot.


## Threads: the signal check went to one word (2026-10-03)

Threads (`clone(CLONE_THREAD)`, `futex`) moved the signal mask and a pending
set onto each thread. The first version made the check on every return to user
mode read both the thread's and the process's 64-bit pending sets — four
loads instead of two. That is the +6 / +4 on the null syscall the section
above saw while this was in progress. The fix is Linux's `TIF_SIGPENDING`: one
32-bit `sig_check` word per thread, set by whoever makes a signal pending and
recomputed by `signal_check()`. It is one load and no pointer chase to the
process, which is cheaper than before threads existed.

`make bench` (QEMU, cycles; "before" is the tree just before threads):

| | before threads | threads, two pending sets | threads, `sig_check` |
|---|---|---|---|
| null syscall, `int 0x80` | 130 | 136 | **127** |
| null syscall, `sysenter` | 158 | 162 | **155** |
| null syscall from musl (netbench `getpid`) | 169 | 173 | **166** |
| thread switch | 97 | 97 | 97 |
| exec + exit (`true`) | 34 971 | 35 059 | 35 052 |
| fork + exit + wait | 51 575 | 51 677 | 51 645 |
| fork + exec + exit + wait | 85 655 | 85 782 | 85 720 |

The fork/exec rows are +0.1–0.2%: the thread list bookkeeping and a few more
fields to reset per thread, at noise level.

Costs of the new paths, by construction (no benchmark yet):
- `futex` wait/wake: a hash (64 buckets keyed by address space + address),
  then a walk of that bucket only. Waking one waiter takes the *oldest*
  (fairness), which walks the whole bucket — fine while buckets hold a
  handful of sleepers.
- `pthread_create` is a `thread_fork` (an 8 KiB kernel stack and a register
  frame copy); no address-space work at all.
- `tkill`/`tgkill` scan the 64-entry thread table — not a hot path.

## The pixel console and `/dev/fb0` (2026-10-03, step 11b)

QEMU's std VGA at 1920x1200x32, console 120x37 with the 8x16 font drawn 2x. All numbers
are `make bench` instruction counts; "text" is the same kernel on `-vga cirrus`, where
no driver binds and the 80x25 text screen stays.

| | text (80x25) | fbcon v1 | glyph unrolled per scale | + skip blank cells on a fresh screen |
|---|---:|---:|---:|---:|
| boot: kernel ready (µs) | 1 230 | 32 779 | 9 471 | **3 313** |
| console: one line + scroll, average | 21 565 | 2 179 802 | 649 569 | 651 138 |
| console: one line + scroll, worst | — | 25 721 168 | 6 938 862 | 6 940 431 |
| fbcon full redraw (120x37) | — | 25 042 183 | 6 754 091 | 6 763 341 |

Slot start jitter (worst) stayed at 234–284 µs in all four runs: the boot console isn't
drawn inside the measured window.

- **v1** drew each glyph pixel by pixel with a loop over the scale: ~25 instructions per
  pixel written. **Unrolling** (`#pragma GCC unroll`, one copy of the glyph routine per
  scale 1-4, chosen by a `switch` once per cell) brings it to ~7 per pixel — close to
  the store itself. fbcon is 4.4 KiB of code.
- **The fresh-screen skip:** QEMU clears video memory on a mode set, so the first
  draw after it needn't write blank black cells. That's the boot console's whole
  screen: kernel ready went from 9.5 to 3.3 ms of CPU.
- **The average scroll** is the new line's 120 cells; scrolling itself is one
  register write (Y panning). **The worst** is when panning reaches the end of video
  memory (every ~30 scrolls at 16 MiB): the whole screen is redrawn at the top, ~9 MiB of
  writes. On a Pentium 4 over PCI/AGP that is a few ms with interrupts off — listed in
  `docs/gaps.md`.
- **Write-combining** (PAT entry 1, 4 MiB pages for the 16 MiB of video memory): a 1 MiB
  framebuffer write is 655 449 instructions WC and 655 448 uncached. QEMU can't show
  the difference; on real hardware uncached video memory takes one bus cycle per 4-byte
  store, WC bursts 32-64 bytes — Linux measured 5-10× for framebuffer writes when it
  switched. Confirm on the target machine (step 12).
- Unchanged: syscalls, context switches, exec/fork (no hot path touched; the mmap path
  checks one more flag only for device files).

## Pre-faulting user buffers (2026-10-03)

`fault_in_user()` touches one byte of every page of a user range before a syscall
looks at any shared state, so the `copy_to_user` that follows cannot fault and
therefore cannot sleep. The reason is in `docs/security.md`: a sleeping copy let two
processes act on one ring-buffer snapshot, which was seven real bugs. This is the
cost of closing the common case.

Measured on a snapshot of the tree with nothing else running (a second QEMU on the
same machine moves these numbers by more than the effect does), the same build twice
each way:

| | without | with | change |
|---|---|---|---|
| `lseek + read 4 KiB` | 1506 | 1506 | — |
| `lseek + read 4 KiB` | **1506** | **1540** | **+34 cycles, +2.3%** |
| `null syscall, int 0x80` | 127 | 127 | none |
| `null syscall, sysenter` | 155 | 155 | none |
| `open + close`, `statx`, `fork + exit + wait` | unchanged | unchanged | none |

Both runs each way gave the identical figure, so the 34 cycles is the effect and not
noise. It buys one page probe: the range check, the call, and one `or byte [addr], 0`.

Where that lands against the rules:

- **Proportional to the request, not to the transfer.** A 4 KiB read probes one page;
  a 256 KiB read probes 64, which is a few hundred cycles against roughly 64 000 spent
  copying — under half a percent. The bad case is the opposite shape: a caller that
  offers a large buffer and receives little, e.g. `read(fd, buf, 65536)` on a pipe
  holding ten bytes, which probes 16 pages for ten bytes. Bounded, but wasteful.
- **`RW_MAX` is what bounds it.** It moved from four separate `#define`s in the
  filesystems into `include/vfs.h` and is now applied in `rw()`, so the clamp happens
  once, before the probe, and no caller can choose how many pages get touched. 64 is
  the worst case for any `read` or `write`; the socket calls use the smaller bound the
  protocol gives (`tcp_buf_max()`, or the datagram maximum) rather than `RW_MAX`,
  precisely so they do not probe pages the call could never reach.
- **It breaks copy-on-write pages eagerly.** A `read()` into a buffer a forked child
  still shares will unshare every page of the request, including pages the short read
  never fills. Not measured; it needs a benchmark that forks and then reads into a
  large buffer, which does not exist yet.

## Memory areas: a growable array (2026-10-03)

A process's memory areas (`struct mm`, `mm/mm.c`) were a fixed array of 32 searched in
order. A program linked against a few dozen shared libraries has hundreds (musl's loader
`mprotect`s each library's RELRO part, which splits an area), so the live disk's programs
died with "RELRO protection failed". The array now starts at 32 and doubles up to 1024,
and lookups are a binary search (O(log n), 10 steps at most) instead of a scan. Growing is
a `kmalloc` + copy inside `mmap`/`mprotect`, never in the page-fault path.

`make bench`, QEMU, cycles (median), same build otherwise:

| | before | after |
|---|---|---|
| `exec + exit` | 35345 | 35259 |
| `fork + exit + wait` | 51703 | 51606 |
| `fork + exec + exit + wait` | 86069 | 85881 |

Unchanged within noise: small programs have few areas, and `fork` copies one array as
before. Not measured: a page fault in a process with hundreds of areas, where the
binary search replaces what would have been a long scan.

## Holding the open file across a syscall (2026-10-04)

`fd_get` returned the `struct file` without taking a reference, so a sibling thread's
`close()` could free it while this one was asleep inside a blocking read — a use-after-free,
with `racetest` able to make a *blocking* read return `EAGAIN` out of a recycled object
(`docs/security.md`). The fix is `file_get`/`file_put` around the call, as Linux pairs
`fdget` with `fdput`: in `rw()` for every `read` and `write`, in the thirteen socket calls
that take an fd, and in `fs/epoll.c`. Separately, `struct file` gained a per-open serial
number so `epoll` can tell a registered file from a different one that landed on the same
freed block.

`make bench`, QEMU, `-icount shift=0` so the numbers are deterministic rather than medians:

| | before | after | |
|---|---|---|---|
| `lseek + read 4 KiB` | 1540 | 1571 | +31 (+2.0%), the `rw()` pin |
| `open + close` (2-part path) | 1482 | 1505 | +23 (+1.6%), the serial |
| `null syscall, int 0x80` | 127 | 127 | — |
| `null syscall, sysenter` | 155 | 155 | — |
| `stat (statx)` | 1300 | 1300 | — |
| `thread switch` | 97 | 97 | — |
| `fork + exit + wait` | 51786 | 51788 | — |
| `exec + exit (ELF 'true')` | 35381 | 35376 | — |

Two percent on the read path is the real cost of this, and it is worth being clear that it
buys correctness rather than anything else — rule 1 of the project is that we never trade
correctness for speed, so the question was only ever how to pay it cheaply.

The serial was 64 bits first, which measured +35 rather than +23. The reason is worth
keeping: `struct file` is 28 bytes and comes from `kmalloc`'s 32-byte size class, and eight
more bytes pushed every open file in the system into the 64-byte class. Thirty-two bits
keeps it where it was and halves the cost, in exchange for a collision that needs the
counter to wrap through 2^32 opens *and* the new file to land on the same address *and* at
the same fd number.

Not measured: the thirteen socket wrappers. `netbench` echoes through `write`/`read`, which
go through `rw()`, so it exercises that pin and not `sendto`/`recvfrom`/`sendmsg`. Those
calls already copy a `msghdr` and an iovec before they do anything, so two increments and a
branch are not where their time goes, but that is an argument and not a number. For the
record, with `tools/echo-server.py` running, after the fixes: UDP `write()` 64 B 1827
cycles, UDP `read()` 484, UDP total per echo 3767; TCP `write()` 1910, `read()` 495, total
per echo 5810.
