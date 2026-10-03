# Notes for Claude

Grok wrote this while you were out of tokens (2026-10-03). You are still the lead.
Concerns and intent, then what landed, then the tests by name.

## Will step 9 get networking up?

No. Step 9 is disks and file systems. Nothing here opens a socket or talks to a
card. The network is step 10 (e1000, then ping).

## Intent: the network stack should look like Linux

Rodger, 2026-10-03: the kernel network stack should be as similar to Linux as we
can make it, so a later port of a network driver is small.

- The part that makes a Linux driver drop in is the boundary the driver already
  calls: `net_device`, `sk_buff`, NAPI, PCI/DMA/IRQ. DESIGN.md already says the
  drivers are unmodified Linux v7.2 source (`e1000` first). Keep that.
- He also wants the stack above that boundary to follow Linux's shape (socket
  layer, `sk_buff` ownership, the same driver callbacks), not a foreign stack
  with an adapter glued on. That closes the old "lwIP or ours, either is fine"
  line. Recorded in DESIGN.md as decided 2026-10-03. It is step 10. It was not
  started.
- Concern: those drivers are GPLv2, and MyOS still has no licence file. Pick one
  before any Linux driver source is copied in.

## What landed

### 9c-2 page cache — done

`fs/pcache.c`, `include/pcache.h`. 128 direct-mapped slots (inode pointer and
page number). A lookup is one array index.

- `file_copy_out` of a disk file goes through the cache. A miss reads that page
  from the file system and also the next page (read-ahead).
- `exec` no longer copies the whole file into a temporary buffer. `elf_map` in
  `kernel/elf.c` reads the headers, maps each PT_LOAD as file-backed VMAs, and
  faults in only the entry page. The rest arrives on first touch. The BSS past
  `p_filesz` stays zeros (`vma.file_end`); a plain mmap still means "through
  EOF" (`file_end == ~0`).
- `ext2_write` and `ext2_truncate` call `pcache_invalidate` before they change
  bytes, so a cached page cannot outlive a write. `inode_free` drops the
  file's pages too.
- A reader pins the slot while it copies, so an invalidate cannot free the
  frame under the copy.
- Concern: the cache is direct-mapped, so two hot pages that hash to the same
  slot kick each other out. 128 slots is enough for a program and its
  libraries; it is not a general cache of every file on the disk. `read()` of
  an ext2 file still uses the block cache directly, not this page cache. Exec
  and `file_copy_out` (mmap faults) use the page cache.
- `make bench` on a RAM root: /init + bash CPU work 6144 µs → 1204 µs. exec of
  the tiny `/bin/true` is 33779 cycles (was 33631): a small file shows the
  fault cost, not a win. The old "18.7 ms from the disk" prompt figure was not
  re-measured. Disk-root bash still waits on the disk for each new page.

### 9e FAT32 in the VFS — read only

`fs/fatvfs.c`. The first partition of type 0x0b or 0x0c is mounted at `/mnt`
(`kernel/main.c`, after the ext2 mounts). Directories are filled on first
lookup. `read()` and the page cache both go through `fat32_read`. Create,
unlink, rename, truncate, setattr, and `write()` return EROFS.

`statfs` on a FAT mount reports `MSDOS_SUPER_MAGIC` (0x4d44). Free-cluster
count is 0 until writing exists. Long names are still ASCII only.

Concern: FAT has no inode numbers. `fs_ino` is the first cluster. An empty
file's cluster is 0.

## Still open in step 9

- 9c-3: the installer (partition, mke2fs, copy, boot area), raw `/dev/hda...`,
  and a UTM VM on the installed layout. Do not point it at
  `dist/MyOS read-write.utm`'s data disk.
- 9d: PCI scan, PIIX/ICH bus-master DMA, AHCI. Keep PIO. ATA is still PIO,
  LBA28, legacy ports 0x1F0/0x170.
- 9e write: allocate clusters, update both FAT copies, directory entries.
  Test it by writing on the stick and reading the image back with mtools,
  the way ext2 is checked with e2fsck and debugfs.
- Later, not the numbered line: ext4 read, exFAT, ext4 write (a journal).
  NTFS stays out. USB disks need a USB host controller first.
- Step 8j `/proc` is still open. It is not part of finishing step 9.

## Tests

All of these passed on 2026-10-03.

- `make test` — `Self-tests: 85 passed, 0 failed`, then `TEST OK` (e2fsck and
  debugfs on the ext2 test disk).
  - `pcache_selftest` (`kernel/pcache_test.c`). The serial log must contain
    `pcache: second read comes from RAM` and
    `pcache: read-ahead filled the next page`. It reads `/data/numbers.txt`
    twice, then the next page (already fetched), then drops the cache and
    reads again.
  - `fatvfs_selftest` (`kernel/fatvfs_test.c`). The serial log must contain
    `fatvfs: HELLO.TXT read through the VFS`. Also checks the long name
    `/mnt/docs/This is a long file name.txt`, `/mnt/src/kernel/sched.c`, the
    first and last byte of `/mnt/data/pattern.bin` (64 KiB), and that
    `vfs_create` on `/mnt` returns EROFS.
- `make test-shell` — bash 5.2 still reaches a prompt and the usual shell
  checks pass. Programs now start by mapping the file.
- `make bench` — numbers above, written into `docs/performance.md`.

## Other concerns

- The boot picture holds about two seconds in stage 2 (`hold` in
  `arch/i386/boot/stage2.asm`). `make test` waits that out. A Desktop trial
  UTM was never built.
- Do not commit unless Rodger asks. This work is uncommitted.
