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
| boot: kernel ready, 64 MiB (µs)   |   11 608 |    763 | CPU work; 119 before BusyBox + bash (~330 initramfs entries, ~1.7 µs each); 9b: +36 finding disks, mounting /data |
| boot: bash prompt, 64 MiB (µs)    |        — |  6 907 | CPU work: kernel ready + 6 144 for /init, ld-musl, bash's libraries, /etc/profile |
| boot: kernel ready, 512 MiB (µs)  |        — |    181 | was 4 181 before the bitmap fix (23×) |
| memcpy 4 KiB                      |   12 315 |  1 053 | 11.7× (`rep movsd`)        |
| memset 4 KiB                      |   10 266 |  1 049 | 9.8× (`rep stosd`)         |
| kmalloc+kfree 64 B                |      109 |     63 | 1.7× (size classes)        |
| … with 1000 objects live          |   15 105 |     63 | 240×, and now constant     |
| thread switch                     |      188 |     94 | 2× (run queue, step 8)     |
| exec + exit (ELF `true`)          |  218 177 | 33 631 | 6.5× (memset, demand paging); 8i: +437, x-bit check, inode refs |
| null syscall, `int 0x80`          |      115 |    128 | +11: registers saved for fork, signal check |
| null syscall, `sysenter`          |        — |    158 | see note                   |
| fork + exit + wait                |        — | 51 403 | step 8 (copy-on-write); 8i: +187, cwd and mapping refs |
| fork + exec + exit + wait         |        — | 84 316 | step 8; 8i: +128           |
| slot start jitter, worst (µs)     |        4 |      4 | —                          |
| disk read 64 KiB, CPU work        |  223 722 | 34 365 | 6.5×: interrupts, 16 sectors per IRQ (was polling every sector) |
| block-cache hit (bread + brelse)  |        — |    101 | step 9: hash lookup, LRU move |
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

- **Revisit**: unpack the initramfs lazily (a directory's entries on first lookup) —
  boot grew from 119 to 713 µs with ~330 files (BusyBox links, bash, libraries).
- **Revisit**: a dynamically linked program's libraries are copied page by page from
  the initramfs on first touch; a page cache shared between processes (step 11) would
  map the same pages into every bash.
- **Revisit**: path lookup got 20% slower in step 8 (symlinks, per-process cwd/fds):
  e.g. skip the `strlen`, cache the last lookup.
- **Step 9**: disk I/O with IRQs (done) + bus-master DMA, read-ahead, a page cache.
- **Step 11**: one page cache for `read()` and `mmap()`, used by ext2 too (today a disk
  file's block is found through ~4 block-cache lookups: inode, indirect blocks, data);
  a real vDSO with `clock_gettime`.
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
