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
| boot: kernel ready (µs)           |   11 608 |    601 | 19× (background calibration) |
| memcpy 4 KiB                      |   12 315 |  1 053 | 11.7× (`rep movsd`)        |
| memset 4 KiB                      |   10 266 |  1 049 | 9.8× (`rep stosd`)         |
| kmalloc+kfree 64 B                |      109 |     63 | 1.7× (size classes)        |
| … with 1000 objects live          |   15 105 |     63 | 240×, and now constant     |
| thread switch                     |      188 |    188 | —                          |
| exec + exit (ELF `true`)          |  218 177 | 44 887 | 4.9× (mostly memset)       |
| null syscall, `int 0x80`          |      115 |    115 | —                          |
| null syscall, `sysenter`          |        — |    145 | see note                   |
| slot start jitter, worst (µs)     |        4 |      4 | —                          |
| open + close (`/data/numbers.txt`) |       — |  1 131 | step 7 baseline            |
| lseek + read 4 KiB (initramfs)    |        — |  1 453 | step 7 baseline            |
| stat (`statx`)                    |        — |  1 044 | step 7 baseline            |

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
12. **Step 7, zero-copy initramfs**: the archive is unpacked once into an inode tree whose
    names and file data point into the archive; path lookup is one hash probe per
    component; `read()` copies straight from the archive into the user buffer; programs run
    from `/bin` with the ELF loader reading the archive in place.

## Designed in, built with its step (binding — see `CLAUDE.md`)

- **Step 8**: copy-on-write `fork`, `vfork` / `CLONE_VFORK`, demand paging, one page
  directory per process (global kernel pages already in place).
- **Step 9**: disk I/O with IRQs + bus-master DMA, read-ahead, a page cache.
- **Step 11**: one page cache for `read()` and `mmap()`; a real vDSO with `clock_gettime`.
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
