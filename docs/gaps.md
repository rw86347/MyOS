# Known gaps

Shortcuts, limits and "later" notes in the current code, in one place. Remove an entry
when it's fixed. File references point at the comment that mentions the gap.

## Boot
- **Kernel must end below 4 MiB** (incl. `.bss`): `entry.asm` maps only one 4 MiB page
  before `paging.c` maps the rest. The kernel is ~33 KiB, so plenty of room.
- **Needs a Pentium Pro or later.** `cpu_init()` checks and prints a message, but
  `entry.asm` already turns on 4 MiB pages (PSE) before that — a CPU without PSE would
  crash before the message. (`arch/i386/entry.asm`, `arch/i386/cpu.c`)
- **No check for BIOS disk extensions** (`int 0x13` AH=0x41) before using LBA reads.
  BIOSes from ~1998 on have them. (`mbr.asm`, `stage2.asm`)
- **Very old BIOSes copy only below 16 MiB** with `int 0x15` AH=0x87 (the 286-era version
  ignores base bits 24-31). Fine while kernel + initramfs fit below 16 MiB; a big
  initramfs (BusyBox + more) on such a machine would need "unreal mode" instead.
- **Boot disk has no partition table**: stage 2, the kernel and the initramfs sit in raw
  sectors after the MBR. A real USB stick would want a partition for the data.
- **E820 map holds at most 32 entries.** (`layout.inc`, `E820_MAX`)

## CPU / interrupts
- **Uses the old 8259 PIC and PIT**; real machines from ~2005 have the APIC. Switch to the
  local APIC timer / IOAPIC later. (`arch/i386/pic.c`)
- **No double-fault task gate.** A kernel stack overflow becomes a triple fault (reboot)
  instead of a message. (`arch/i386/interrupts.c`)

## Memory
- **RAM above 768 MiB is ignored** ("highmem"). Fine for old PCs with ≤ 768 MiB.
  (`mm/pmm.c`, `arch/i386/include/memlayout.h`)
- **Kernel is one read/write/execute blob.** Code isn't write-protected and data isn't
  no-execute. The Makefile comment says to split permissions once paging is on — paging
  *is* on now, so this is due. (`Makefile`, `arch/i386/linker.ld`)
- **Frame allocator is a linear bitmap scan.** Slow with lots of RAM; fine for now. (`mm/pmm.c`)
- **The heap never returns pages to the PMM.** Size-class pages stay with their class, and
  the big-block range never shrinks. Freed memory is reused, but never goes back to the
  page pool. (`mm/kheap.c`)
- **Big blocks (> 1 KiB) are first-fit with a walk** over the big blocks. They're rare
  (thread stacks, buffers); small objects are O(1). (`mm/kheap.c`)
- **`kfree` can't detect a double free of a small object** (no per-object flag). It would
  corrupt that class's free list. (`mm/kheap.c`)
- **Nothing above 0xFF000000 is used yet** (planned for temporary mappings etc.).
  (`memlayout.h`)

## Threads / scheduler
- **No locks.** Preemption is on, so two threads can be inside the same code at once.
  `kmalloc`/`kfree` and the disk drivers aren't protected. (Console output is: each piece
  of text is written with interrupts off.) Syscalls run with interrupts off, so user programs
  can't collide; kernel threads could. Fix: `irq_save` sections (one CPU) or a mutex.
- **Round-robin mode still exists** for boot and the thread self-tests (before
  `sched_start()`). (`kernel/sched.c`)
- **Dead threads are reaped lazily**: their slot and stack are only freed when
  `thread_create` needs the slot. (`kernel/thread.c`)
- **Fixed tables: 64 threads, 64 processes, 32 fds and 32 memory areas per process.**
  (`THREAD_MAX`, `PROC_MAX`, `PROC_FILES`, `MM_AREAS`)
- **One thread per process**: `clone(CLONE_THREAD)` returns `-ENOSYS`, no `futex`.
  `vfork` is plain fork (copy-on-write makes it cheap), and `clone(CLONE_VM)` gets a copy.
- **No stack overflow detection.** A thread that uses more than 8 KiB silently overwrites
  the heap. Fix: guard pages (an unmapped page below each stack).
- **The slot table is compiled into the kernel.** Today `config/slots.conf` is turned into a
  C array at build time (`tools/slots2c.awk`), so changing it means rebuilding. Goal: edit
  `slots.conf` on the disk, reboot, and the kernel reads the file at boot and runs the new
  table — no rebuild. Needs a file system the kernel can read early (FAT32 already reads; or
  the initramfs, step 7) and a small parser in the kernel with the same checks as the awk
  script (slots 1/10 reserved, one owner per slot), falling back to the built-in table if
  the file is missing or wrong.
- **Slot tasks are created by the self-test**, not from the config: the kernel only knows
  names, and there are no programs to load yet. Later a name should map to a program
  (an ELF file) that the kernel starts at boot.
- **A slot owner can't run longer than 1 ms at a time, and overruns aren't reported.** It's
  simply preempted at the slot end. Real-time systems usually log or count overruns.
- **The first slot after `sched_start()` is shorter than 1 ms** (the frame starts mid-tick).

## Processes, signals, terminal (step 8)
- **No job control stops**: SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU are ignored, so Ctrl-Z does
  nothing and `fg`/`bg` can't work. No SIGTTOU/SIGTTIN for background terminal access.
- **No syscall restart**: a signal makes a sleeping syscall return `-EINTR` even with
  `SA_RESTART`. No `sigaltstack`, no core dumps, `pselect`/`ppoll` ignore their mask.
- **One terminal**: the console (VGA + keyboard). Serial input isn't read; `/dev/ttyN` are
  all the console. VT100 subset only (no scroll regions, no line drawing).
- **`poll`/`select`**: every waiter is woken on any terminal/pipe change (fine for a few
  waiters); `select` handles fds 0-31 only.
- **Time**: millisecond resolution; the CMOS clock is assumed to be UTC; no
  `settimeofday`, no `alarm`/`setitimer`/timers.
- **No writable file system**: everything that would change files returns `-EROFS`
  (bash can't save history, `/tmp` is read-only, `>` redirections to files fail). A tmpfs
  is the next storage step.
- **No networking**: socket calls fail (`getpeername` says "not a socket").
- **`uname` says "Linux"** (programs check it to know which syscalls exist); the release
  is "6.1.0-MyOS". `getrandom` is not cryptographic (no entropy source).
- **`mprotect(PROT_NONE)` doesn't hide pages**: present user pages stay readable on x86
  without PAE (`PROT_NONE` would need unmapping).
- **No address-space randomisation**: static PIEs always load at 0x00400000.
- **Every program runs as root** (uid 0).
- **Programs and mmap'd files only come from the initramfs** (`inode->data`); disk file
  systems will need a page cache. File mappings are private copies (made per page on first
  touch); `MAP_SHARED` file mappings behave like `MAP_PRIVATE` (nothing can be written
  back anyway).
- **Dynamic linking**: one interpreter level; the interpreter is put at the first free
  place in the mmap area (no randomisation).
- **Arguments + environment: 64 KiB, 1024 entries; paths 255 bytes** (Linux: 128 KiB+, 4096).
- **Syscalls run with interrupts off** (interrupt gate); long ones are capped (reads at
  256 KiB, console writes return short). Needs locks to change.
- **Execute permission isn't enforced** (no NX bit without PAE).
- **CPU exceptions become signals** (SIGSEGV/SIGFPE/SIGILL); a handler for them is called
  but the faulting instruction runs again when it returns (like Linux).

## Disk / file systems
- **ATA driver polls** instead of using IRQs 14/15, even though interrupts work now (the
  comment in `drivers/ata.c` still says "we don't have interrupt handling yet").
- **ATA is PIO only**; no DMA. Slow, but always works. (`drivers/ata.c`)
- **LBA28 only**: disks up to 128 GiB. LBA48 is a small addition. (`drivers/ata.c`)
- **MBR partitions only**: no extended/logical partitions, no GPT. (`fs/partition.c`)
- **FAT32 is read-only.** (`fs/fat32.c`)
- **FAT32 long names are ASCII only**; other characters become `?`. (`fs/fat32.c`)
- **Block devices live in a fixed-size array.** The comment says "no kmalloc yet" — we have
  kmalloc now. (`kernel/blockdev.c`)
- **The VFS knows only the initramfs (read-only) and the console.** No ext2 yet (step 9),
  FAT32 isn't plugged in, nothing can be written or created (`-EROFS`). (`fs/vfs.c`)
- **Missing for a shell (step 8):** `getdents64` (listing a directory), `chdir` / a current
  directory (relative paths start at `/`), `openat`/`statx` relative to a directory fd
  (`-ENOSYS`), following symbolic links, `dup2`, `fcntl`.
- **Reading the console returns end-of-file**: there's no way to wait for a key yet (needs
  sleeping/blocking, step 8). (`fs/vfs.c`)
- **One `read()` copies at most 256 KiB** (so a syscall never runs long with interrupts
  off); programs and C libraries loop on short reads. (`fs/vfs.c`, `READ_MAX`)
- **Open files belong to the thread** (`thread->files`, 32 fds); they become per process
  with fork in step 8. Hard links in the cpio archive aren't merged.
- **Paths are limited to 255 bytes** (Linux: 4096). (`include/vfs.h`, `PATH_MAX`)
- **The initramfs is built at compile time**: editing a file in it means `make` again
  (the kernel itself isn't rebuilt, only the archive and the disk image).
- **The "USB stick" is an IDE disk in QEMU**: there's no USB stack.

## Drivers
- **Keyboard ignores extended keys** (arrows, Home/End, right Ctrl/Alt…). Scancodes are
  parsed correctly, just not mapped. (`drivers/keyboard.c`)
- **Keyboard drops keys when its ring buffer is full.** (`drivers/keyboard.c`)
- **The serial output queue is 8 KiB.** Kernel messages that overflow it make `kprintf`
  wait for the port (rare); user `write()`s get a short count instead.
- **The cycle counter is calibrated during the first ~12 ms after boot**; anything asking
  for `cycles_per_us()` before then waits. It assumes the TSC ticks at a constant rate.
  On a Pentium M (and early Pentium 4s) it follows SpeedStep's clock changes, so for
  measurements there, turn SpeedStep off in the BIOS. Nothing uses it for timekeeping yet.
- **Keyboard: US layout, scancode set 1 only.**
- **All drivers are in the kernel**; the plan is to move most to user space. (`docs/DESIGN.md`)
