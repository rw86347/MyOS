# MyOS Design Decisions

Decisions that shape later roadmap steps. Add open questions at the bottom of each section.

## Boot: our own BIOS/MBR bootloader (no GRUB)
Two stages; disk layout `[ MBR | stage 2 (8 sectors) | kernel.bin | initrd.cpio ]`.
- **Stage 1** (`arch/i386/boot/mbr.asm`, 512 bytes): the BIOS loads it at 0x7C00; it loads
  stage 2 to 0x7E00 with `int 0x13` AH=0x42 and jumps there.
- **Stage 2** (`arch/i386/boot/stage2.asm`): reads the BIOS memory map (`int 0x15` E820),
  enables A20, loads the kernel to **1 MiB** and the initramfs right after the kernel's
  `.bss`, leaves both in `BOOTINFO` (0x1000), enters protected mode, jumps to `_start`.
- Loading above 1 MiB from real mode: read 32 KiB into a bounce buffer at 0x10000, then
  copy it up with `int 0x15` AH=0x87. BIOS-only, so it also works from USB/CD on real
  hardware. No size limit (decided 2026-10-02; Multiboot/GRUB not needed).
- The Makefile passes the sector numbers and load address to stage 2 with `-D`.
- The kernel (incl. `.bss`) must end below 4 MiB: `entry.asm` maps only the first 4 MiB
  (one PSE big page) until `paging.c` maps the rest.

## Scheduling: fixed time-slot table (time partitioning)
Like ARINC 653 in avionics: time is divided into a repeating **major frame**.
- Major frame = **20 slots × 1 ms** = 20 ms (both numbers configurable).
- Slots are numbered **1–20**. Slots **1 and 10 are system slots** (kernel / system user-space apps).
  The other **18 slots** can be assigned to **named** tasks.
- A real-time task gets a fixed set of slots, so its timing is guaranteed. Example:
  `"anti-lock brakes"` → slots 4, 9, 14, 19 (runs every 5 ms, 1 ms each).
- Configured in a text config file (e.g. `config/slots.conf`). Until the kernel has a
  filesystem, the build turns the file into a C table compiled into the kernel; later the
  kernel can read it from the initramfs.
- Timer: PIT (later local APIC timer) programmed for a 1 ms tick; each tick = next slot.

Open questions:
- What runs in an unassigned slot, or when a task finishes its slot early? (Idle? Best-effort
  "background" tasks round-robin?)
- Can a name own multiple processes/threads?
- What happens if a task overruns? (Preempt hard at slot end — the usual answer.)

## Drivers: in user space, Linux-compatible
- Goal: most drivers run as user-space processes (microkernel-style). The kernel provides
  what they need: mapping device memory (MMIO) and I/O ports, delivering IRQs, DMA buffers.
- "Linux-compatible" means **source-level**: Linux drivers are written against Linux's
  *internal* kernel API (no stable ABI), so binary `.ko` modules can't be loaded. Instead,
  a shim library re-implements the Linux kernel API the drivers use, and unmodified (or
  lightly patched) Linux driver source is compiled against it.
  Prior art: FreeBSD **LinuxKPI** (in-kernel), TU Dresden **DDE** (user space), NetBSD **rump**.
- Early drivers (serial, VGA, PIT, keyboard) stay in the kernel until user mode exists.

## Compatibility policy: "Linux without the legacy"
What has to match Linux is the **boundary** that programs and disks see. The inside of the
kernel can be our own design.
- **Must match exactly:** syscall numbers, argument order, struct layouts (`struct stat64`,
  `struct statx`, `linux_dirent64`, ...), errno values, file semantics (permissions, `O_*` flags),
  and on-disk formats (ext2/ext4, FAT32, MBR/GPT partition tables).
- **Can be dropped (return `-ENOSYS`):** Linux i386 still carries old syscall versions for
  1990s binaries. Examples: old `stat`/`oldstat` (use `statx`/`fstatat64`), 16-bit-uid
  `setuid`/`getuid` (use the `*32` versions), 32-bit-`time_t` calls (use the `*_time64`
  versions), `socketcall` (use direct `socket`/`bind`/...), `oldolduname`, `vm86`, `uselib`,
  `sysfs`, a.out binaries.
- **Rule for what to implement:** implement what **recent musl** actually calls (check musl's
  source for each syscall), then glibc as needed. Every syscall we don't implement logs its
  number and name to serial once. Real programs show us what's missing.
- Honest note on speed: dropping legacy syscalls makes the kernel *smaller and simpler*, not
  much faster. Linux's per-syscall cost is already low. Our speed wins come from a smaller code
  path, the fixed slot scheduler (predictable latency), and zero-copy I/O paths (below).
  Linux's throughput will be hard to beat; guaranteed timing is where we can win.

## Storage: disks and file systems
Layers, bottom to top:
```
  program     open/read/write/stat/getdents64/mmap     (Linux syscall ABI)
  ----------------------------------------------------------------------
  kernel      VFS  (superblock, inode, dentry, file — same concepts as Linux)
              page cache  (file data cached in 4 KiB pages; also used by mmap)
              file systems: initramfs (cpio), ext2, ext4 (read then write), FAT32, exFAT
              not planned: NTFS
              partition parsing: MBR, later GPT
              block layer: request queues, one shared-memory ring per disk
  ----------------------------------------------------------------------
  user space  block drivers: ATA PIO -> ATA DMA (PIIX/ICH), AHCI (SATA), virtio-blk (QEMU),
              later USB mass storage
```

Order of work (fills in roadmap steps 7 and 9):
1. **VFS + initramfs**: a RAM-only filesystem loaded with the kernel. No disk driver is needed,
   so the file syscalls and ELF loading can be tested early.
2. **Block layer + ATA PIO driver**: the simplest disk interface (CPU copies every byte). Slow
   but about 200 lines, and works in QEMU and on every IDE-era PC.
3. **MBR partition table** parsing (we already write MBRs; GPT later).
4. **ext2 read**, then **ext2 write**: the classic Unix filesystem and the best documented.
5. **ext4 read** (extents, 64-bit block numbers, `dir_index`): modern Linux disks use ext4.
6. **ext4 write.** In scope. A crash must not corrupt the disk, so this waits on a journal
   (replay the log on mount, commit data and metadata through it). It comes after ext4 read.
7. **FAT32**: USB sticks and moving files between machines. Read-only already works.
   A file cannot be bigger than 4 GB, and the volume itself tops out around 32 GB in practice.
8. **exFAT**: the same job once a stick is bigger than 32 GB or a file is bigger than 4 GB.
   This is what Windows and cameras write now.
9. Faster drivers: **ATA DMA / AHCI** for real hardware, **virtio-blk** for QEMU.

Decided (2026-10-02): **exFAT and ext4 write are in scope. NTFS is not.** We will not mount
Windows system disks.

Driver compatibility for disks: the Linux disk drivers (libata, AHCI) are large and tied
to Linux's SCSI layer, so a Linux-API shim for them is more work than writing small native
drivers. Plan: native drivers for storage (ATA/AHCI/virtio are well documented and small);
use the Linux shim where writing our own would be impractical (network cards, WiFi, USB
host controllers).

Decided (2026-10-02): **network drivers come from Linux, not from scratch.** NIC (and later
WiFi and USB host) drivers are unmodified Linux v7.2 source compiled against the shim:
`e1000` first (QEMU's card; no firmware, no `phylib`), then `e100`, `8139too`, `r8169`. The
shim provides `net_device`, `sk_buff`, NAPI, PCI/DMA/IRQ, timers, workqueues and locks —
a few thousand lines before the first driver runs, after which each new driver is cheap.

Decided (2026-10-03): the stack above that boundary follows Linux's shape too
(`sk_buff` ownership, the socket layer, the same driver callbacks), so a ported
driver is not glued onto a foreign stack. This is step 10. Step 9 does not
bring the network up. The earlier "lwIP or ours" choice is closed.
Licence note: those drivers are GPLv2, so a MyOS image that includes them is effectively
GPLv2 too; MyOS has no licence file yet — pick one before any Linux driver source is copied in.

Decided (2026-10-03, later the same day): the first e1000 and the IPv4 path are
original code, written against that same `net_device` / `sk_buff` / NAPI boundary,
because the licence file is still missing. Porting a Linux driver later stays
possible. "Better than Linux" is not a measured claim; a number from `make bench`
has to come first, and a big TCP transfer is where Linux stays ahead.

Decided (2026-10-01): **file systems and the VFS run in the kernel**, so every `read()` stays
one syscall with no extra context switches. (Block *device drivers* still move to user space.)

Decided (2026-10-03): **the root file system.** The kernel itself mounts the ext2
partition labelled `myos-root` as "/" at boot (no switch_root program, no driver
initramfs): disk drivers are in the kernel. Without one — a live USB stick or CD — MyOS
runs from the initramfs in RAM. `myos-data` is mounted at /data. Other ext2 disks are
left alone. Storage drivers (IDE, AHCI, later NVMe) stay in the kernel: one syscall per
read, no extra context switches; the user-space driver ring is for shim drivers (NICs,
WiFi, USB host).

Open questions:
- Block drivers in user space need a fast path: kernel and driver share a request ring and the
  data buffers (no copying), and an IRQ wakes the driver. Can a disk driver own one of the
  system slots (1 or 10), or does it need its own slots?
- Root filesystem at boot: initramfs only at first. Later, do we mount ext4 from disk with an
  initramfs that holds the drivers (Linux style)?

## Unchanged goals
- Linux i386 syscall ABI (`int 0x80`), ELF, static musl binaries first.
