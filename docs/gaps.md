# Known gaps

Shortcuts, limits and "later" notes in the current code, in one place. Remove an entry
when it's fixed. File references point at the comment that mentions the gap.

## Boot
- **Kernel must end below 4 MiB** (incl. `.bss`): `entry.asm` maps only one 4 MiB page
  before `paging.c` maps the rest. The kernel is ~33 KiB, so plenty of room.
- **Needs a Pentium or later** (4 MiB pages / PSE, `cpuid`), and doesn't check: on a 486
  it would crash at boot instead of printing a message. (`arch/i386/entry.asm`)
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
- **The heap never returns pages to the PMM.** Freed heap memory is reused by kmalloc but
  never goes back to the page pool. (`mm/kheap.c`)
- **Heap is first-fit with a linear walk.** Linux uses slab/SLUB. (`mm/kheap.c`)
- **Nothing above 0xFF000000 is used yet** (planned for temporary mappings etc.).
  (`memlayout.h`)

## Threads / scheduler
- **No locks.** Preemption is on now, so two threads can be inside the same code at once.
  `kmalloc`/`kfree`, `kprintf` (VGA cursor, serial) and the disk drivers aren't protected.
  The self-tests avoid this (spinners don't print or allocate). Fix: spinlocks (`irq_save`
  for now, since we have one CPU) or a mutex, starting with the heap and kprintf.
- **Round-robin mode still exists** for boot and the thread self-tests (before
  `sched_start()`). (`kernel/sched.c`)
- **No way to sleep or block.** A thread waiting for a key or for time still gets its
  slots (it just `hlt`s through them).
- **Dead threads are reaped lazily**: their slot and stack are only freed when
  `thread_create` needs the slot. (`kernel/thread.c`)
- **Fixed table of 16 threads.** (`include/thread.h`, `THREAD_MAX`)
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

## User mode / syscalls (step 5)
- **One shared address space.** All user programs use the kernel's single page directory,
  so only one program can be loaded at a time (`user_exec` / `user_cleanup`). Needs a page
  directory per process — comes with fork/exec (step 8). (`kernel/usermode.c`)
- **User programs don't get time slots.** They run in round-robin mode before the slot
  scheduler starts. A program should be a named slot owner in `slots.conf`.
- **A user program that never exits hangs the boot**: the test waits for it with
  preemption off, and there's no `kill` or timeout.
- **Programs are embedded in the kernel image** (`user/programs.asm`), not read from the
  initramfs yet (step 7).
- **Static, fixed-address ELF only** (`ET_EXEC`). No static-PIE (`ET_DYN`), no dynamic
  linking / `PT_INTERP` (step 11). (`kernel/elf.c`)
- **The environment (`envp`) is always empty**, and `argv` is capped at 16 entries.
- **`AT_RANDOM` isn't random**: it's derived from the uptime. There's no entropy source.
- **The user stack is a fixed 64 KiB** and doesn't grow. Linux grows it on demand (page
  faults just below the stack).
- **Execute permission isn't enforced.** 32-bit x86 paging has no "no-execute" bit (that
  needs PAE), so data and stack pages are executable.
- **FPU/SSE errors kill the program like a segfault** (exit code 139). Linux would send
  `SIGFPE`; there are no signals yet (step 8). Only happens if a program unmasks FPU
  exceptions. (`arch/i386/interrupts.c`)
- **Only `write` (to fd 1/2), `exit` and `exit_group` exist.** Unknown calls return
  `-ENOSYS` and are logged to serial. (`kernel/syscall.c`)
- **Syscalls run with interrupts off** (interrupt gate). A long syscall delays the timer
  and the slot schedule. A trap gate would keep interrupts on, but needs locks first.
- **`write` prints one character at a time through `kprintf`.** Slow, and not locked.
- **CR0.WP is off**, so the kernel can write to read-only pages. Turn it on with
  copy-on-write (fork) — Linux does.

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
- **No VFS and no ext2 yet** (roadmap steps 7 and 9). The initramfs is loaded and checked,
  but nothing reads files from it yet.
- **The initramfs is built at compile time**: editing a file in it means `make` again
  (the kernel itself isn't rebuilt, only the archive and the disk image).
- **The "USB stick" is an IDE disk in QEMU**: there's no USB stack.

## Drivers
- **Keyboard ignores extended keys** (arrows, Home/End, right Ctrl/Alt…). Scancodes are
  parsed correctly, just not mapped. (`drivers/keyboard.c`)
- **Keyboard drops keys when its ring buffer is full.** (`drivers/keyboard.c`)
- **Keyboard: US layout, scancode set 1 only.**
- **All drivers are in the kernel**; the plan is to move most to user space. (`docs/DESIGN.md`)
