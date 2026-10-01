# Performance plan

Ways to make MyOS fast and efficient, ranked by payoff. Each item says what to change,
why, roughly how much work it is, and which roadmap step it fits into.

**A note on goals.** CLAUDE.md says *"keep the code small and readable over clever or
fast"*. Most of the items below keep that rule: they are design choices that are simple
*and* fast. The ones that make code harder to read are marked **(complex)**.
`docs/DESIGN.md` already says it honestly: beating Linux's *throughput* is unlikely.
MyOS can win on being **small** (fits in old CPUs' caches), **predictable** (the slot
scheduler), and **low-latency**.

---

## 0. Measure first

You can't make something fast without measuring it, and guessing is usually wrong.

- **A cycle counter.** Every Pentium has `rdtsc` (Read Time-Stamp Counter). Calibrate it
  against the PIT at boot to get nanoseconds. Small, and needed by everything below.
- **A few benchmarks**, printed by `make test`:
  - null syscall round trip (user → kernel → user),
  - thread switch,
  - interrupt latency and **slot jitter**: how late each slot really starts. This is
    the number that matters most for the slot design.
  - page fault, and loading an ELF file.
- **The QEMU caveat.** On an Apple Silicon Mac, QEMU *emulates* an i386 CPU in software.
  Its timings don't match a real Pentium 4 at all, and some slow things, like the serial
  port, are instant in QEMU. Use QEMU only to *compare* two versions (`-icount` gives
  repeatable instruction counts). Real numbers need the real machine (step 12).
- **Test with the target CPU.** Boot QEMU with `-cpu pentium3` or `-cpu coreduo` (close
  to a Pentium M), so the kernel never accidentally relies on a feature the old machines
  don't have. Check features with `cpuid` at boot.

## 1. Quick wins (do soon — small and readable)

1. **Console output stalls the whole machine on real hardware.** `serial_putc` waits for
   the UART before every byte (`drivers/serial.c`). At 115200 baud that's about **87 µs
   per byte**. Syscalls run with interrupts off, so a user program printing 100 bytes
   freezes the CPU for about 9 ms. That's 9 missed slots, which breaks the slot
   scheduler's timing guarantee. QEMU hides this because its serial port is instant.
   VGA adds 4 slow port writes per character to move the cursor (`update_cursor`).
   *Fix:* write output into a ring buffer and return immediately. The serial "transmit
   empty" interrupt (IRQ 4) drains the buffer. Move the VGA cursor once per `write()`,
   not once per character. Kernel-panic output stays synchronous, so it's never lost.
2. **Fast `memcpy` / `memset`.** They copy one byte per loop step (`kernel/string.c`).
   Every page the kernel zeroes or loads (ELF segments, page tables, user stacks) goes
   through them. Use `rep movsd` / `rep stosd` (4 bytes per step, and P6-and-later CPUs
   run "fast strings" in hardware). About 10 lines; still readable.
3. **Self-tests run on every boot**, costing about 0.4 s: a 100 ms timer test, 50 ms of
   preemption, 200 ms of slots, and printing the whole FAT disk. Build them only for
   `make test` (`-DSELFTEST`), so a normal boot goes straight to work.
4. **Slot owner lookup does string compares every tick** (`sched_pick` → `thread_find`).
   Cheap today. A `struct thread *slot_thread[21]` table filled at `sched_start` /
   `thread_create` makes it O(1). Tiny.

## 2. Architecture wins (plan them into the roadmap)

5. **Big pages for the kernel (PSE) + global pages (PGE).** ~~PSE~~ **done (7a,
   2026-10-02):** the direct map and the boot mapping use 4 MiB pages — no page tables
   except one for a partial last 4 MiB, far fewer TLB entries. Still to do: global pages
   (PGE, Pentium Pro and later) keep kernel TLB entries when switching processes — only
   useful once each process has its own page directory. → step 8.
6. **`sysenter` + a vDSO.** `int 0x80` is slow on the Pentium 4; Linux added `sysenter`
   in 2002 largely because of it. Both P4 and Pentium M have `sysenter`. Linux gives every
   program a small code page (the vDSO) and passes its address in the auxiliary vector
   (`AT_SYSINFO`). **musl and glibc on i386 call syscalls through it automatically**, so
   unmodified programs get faster syscalls with no changes. The vDSO can also do
   `clock_gettime` without entering the kernel at all. (Moderate work.) → after **6b**.
7. **Copy-on-write `fork`, demand paging, and `vfork`/`CLONE_VFORK`.** A shell runs
   `fork` + `exec` for every command. Copying the whole address space just to throw it
   away is the classic waste. COW shares pages until someone writes. musl's
   `posix_spawn` uses `clone(CLONE_VM|CLONE_VFORK)`, which copies nothing.
   → **Step 8** (design it in from the start).
8. **User memory access via exception fixups.** `vmm_user_ok()` walks the page tables
   before every user buffer access. Linux just does the access, and if it faults, an
   "exception table" sends the fault to an error path (`-EFAULT`). That's faster, and
   it's also *correct* once threads share memory: another thread could unmap the buffer
   between the check and the use. **(complex)** → step 8.
9. **Zero-copy reads from the initramfs.** The initramfs is already in RAM, so
   `read()` can copy straight from the archive into the user buffer, with no cache in
   between. Later the page cache backs both `read()` and `mmap()`, so file data exists
   once. → **Step 7**, then step 11.
10. **Disk: IRQs + bus-master DMA, read-ahead.** ATA PIO polling keeps the CPU copying
    every word and spinning. PIIX/ICH chipsets (exactly the target era) support
    bus-master DMA. Reading ahead hides seek time. → **Step 9**.
11. **Kernel heap with size classes** ("slab-lite"): one free list per size (16, 32, 64…
    bytes). O(1) instead of a first-fit walk (`mm/kheap.c`). Matters once the VFS
    allocates many small objects. → step 7–8, when profiling shows it.
12. ~~**FPU/SSE state, done right.**~~ **Done (2026-10-02).** User threads save and
    restore the FPU/SSE registers eagerly with `fxsave`/`fxrstor` (`arch/i386/fpu.c`).
    The kernel is built with `-mgeneral-regs-only`, so kernel threads skip it. No lazy
    FPU (LazyFP). Possible later win, if benchmarks show it matters: skip the restore
    when the same user thread runs again with nobody else touching the FPU in between.

## 3. Latency — where MyOS can actually beat Linux

13. **Short, preemptible syscalls.** Syscalls run with interrupts off (an interrupt
    gate), so a long syscall delays the next slot. Either keep every syscall short and
    bounded, or switch to a trap gate (interrupts on) — which needs locks first (see
    `gaps.md`). Measure slot jitter (item 0) before and after.
14. **Skip ticks nobody needs** (power, not speed). Program the timer in one-shot mode
    for the next *owner change* instead of every 1 ms, so runs of idle slots cost one
    interrupt. Best with the local APIC timer. Later.

## 4. Not worth it (or be careful)

- **Micro-tuning for QEMU.** Its timings aren't real; tune on the real machine.
- **`-O3`, hand-written assembly beyond `memcpy`, clever data structures** before a
  benchmark says they matter. They cost readability and usually win nothing.
- **Lazy FPU switching** — insecure (see 12).
- **Compiler tuning** (`-mtune=pentium4`, LTO): a few percent at most. Fine later.
- **User-space drivers are slower than in-kernel drivers.** Every I/O costs extra
  context switches. The plan in `DESIGN.md` (shared request rings, batching) limits the
  cost, but it's a trade: isolation vs. speed. Decide per driver; keep the hottest ones
  (timer, maybe disk) in the kernel.

## Additional suggestions from Grok

The machine this has to scream on is a Pentium 4 or a Pentium M. Each line points at an
existing item so this is not a second plan.

Speed on a Pentium 4 is the level-2 cache, not the RAM. Common Pentium 4 chips have
256 KB, 512 KB, or 1 MB of that cache. The PC's RAM (often 256 MB) only decides whether
the kernel fits in the machine. A kernel that fills the RAM will boot and will be slow,
because almost none of it sits in the cache.

What has to fit in those few hundred kilobytes is the code that runs all the time: the
timer tick, the system-call entry, and the task switch. The rest of the kernel can be
larger and stay in RAM, as long as a tick does not drag it in. The kernel is about
21 KB today, so the busy part fits. When a feature is added, check the size of those
three paths. If they grow past the cache, the Pentium 4 will stall on every tick. The
number to print on the real chip is how late each slot starts. Dropping old syscalls,
and turning the compiler optimizer up, will not do it.

The Pentium 4 is the picky one: it is slow to enter the kernel with `int 0x80`, slow to
take an interrupt, and slow when the hot path jumps around. The Pentium M is less picky
and still has a small cache. The same rules apply to both.

1. **Time it on a real Pentium** (item 0). Add a clock read and print how late each slot
   starts. QEMU on the Mac will not give a true number. Boot with `-cpu pentium3` or
   `-cpu coreduo` so the kernel never uses an instruction those chips lack.
2. **Let printing happen in the background** (item 1). Put the letters in a buffer and
   return immediately. Send them when the serial chip is ready. Today each letter waits
   with interrupts off, so the clock cannot run and slots are missed. A wasted interrupt
   is expensive on a Pentium 4.
3. **Copy memory a word at a time** (item 2). Loading a program and clearing a page both
   go through `memcpy` / `memset`, which still move one byte per step. This chip feels
   that on every page.
4. **Starting a program must not copy all of its memory first** (item 7). A shell does
   this for every command. Share the pages until someone writes, and support the musl
   path that copies nothing.
5. **A file already in RAM is read straight into the program** (item 9). One copy. A
   second copy misses the small cache.
6. **Keep the busy code inside the Pentium 4 cache, and keep system calls short**
   (item 13). The timer tick, the system-call entry, and the task switch together stay
   under the level-2 cache (256 KB on the smaller Pentium 4s). A system call runs with
   interrupts off, so a long one makes the next slot start late. An extra layer on
   every call pulls more code into that cache and is felt on this chip first.

Also already listed, and required on this chip: disk DMA instead of the CPU copying
every word (item 10), `sysenter` instead of `int 0x80` (item 6 — this is the Pentium 4
one), and 4 MiB kernel pages so a process switch keeps the kernel in the TLB (item 5).
A driver in its own process adds a switch on every I/O, and a switch is costly when the
cache is this small. The timer, and the disk, stay in the kernel.
