# Notes for Claude

Grok wrote this while you were out of tokens (2026-10-03). You are still the lead.
Concerns and intent, then what landed, then the tests by name.

## Step 10, the card answers a ping (10b)

Rodger asked for a stack written here, safer and faster than Linux, with a
driver interface close enough to Linux that a later port is small. What is
true today:

- The calls match Linux: `net_device`, `sk_buff`, `ndo_start_xmit`, NAPI.
  `drivers/e1000.c` is original (Intel's public register manual). It is not
  Linux's e1000.c. Do not copy that file in until there is a licence.
- A frame goes out the QEMU e1000 and a reply comes back. Static address
  10.0.2.15/24, ARP, ICMP echo to the gateway 10.0.2.2. `make test` greps
  `net: ping 10.0.2.2 ok`. 2026-10-03: Self-tests 96 passed, 0 failed.
- This is not faster than Linux. Nothing has been timed with `make bench`.
  A small stack can be tighter (fixed rings, no allocate in the IRQ, drop
  IP options and fragments, check lengths and checksums, 8-slot neighbor
  table). Linux will stay ahead on a big TCP transfer. Do not claim a win
  without a number.
- UDP, DNS, and a TCP client are in (`net/udp.c`, `net/dns.c`, `net/tcp.c`).
  `make test` greps `net: dns example.com is ` and
  `net: tcp example.com HTTP ok`. 2026-10-03: Self-tests 99 passed, 0 failed.
  That HTTP check needs the host to reach example.com:80. TCP is one client
  connection, one segment in flight, no listen, no out-of-order data.
- 10d, sockets: `net/socket.c` has UDP and raw ICMP sockets, both the direct
  syscalls (359-373) and `socketcall` (102). musl uses the direct ones.
  `alarm`/`setitimer` live in `kernel/signal.c` (sorted list, tick checks the
  head). `make test-shell` greps `2 packets transmitted, 2 packets received`
  (`ping -c 2 10.0.2.2`) and `PING example.com (` (musl DNS through
  `/etc/resolv.conf`). Pinging google.com gets no reply because this network
  drops ICMP to the internet; the Mac cannot ping it either.
- Waiting: socket and kernel network waits use `net_sleep` (sleeps on
  `net_wait`, woken by the e1000 interrupt). Never `cpu_wait` there: syscalls
  run with interrupts off, and `hlt` would never wake. `net_rx_action` refuses
  a nested call (`rx_busy`), so an ARP lookup started from inside receive
  cannot get its answer. Nothing does that today.
- Concern: every socket wakes `poll_wait`, so all `poll` sleepers re-scan.
  Fine for a few processes.
- Benchmark: `user/netbench.c` (MyOS: `make bench`, needs `tools/echo-server.py`
  running; Linux: `tools/linux-netbench.sh`). Results in docs/performance.md.
  Instruction counts, not cycles. Do not claim real-hardware wins from them.
- The e1000 no longer waits in transmit for QEMU's 1 s receive hold. ARP
  retries (3 x 500 ms) cover it. Never sleep_ms() in a path a syscall reaches.
- Security gaps to close before calling the stack secure: docs/gaps.md,
  "Network security". A CSPRNG is the first dependency.
- 10e: TCP sockets are in (`net/tcp.c`, one `struct tcp_conn` per socket;
  `net_thread` in netdevice.c). Concern: the net thread only runs once the
  scheduler does; before `sched_start` (bench, early tests) TCP progresses only
  inside syscalls, which call net_rx_action themselves. Test: `make test-shell`
  greps `wget ok`; `make test` keeps `net: tcp example.com HTTP ok`.
- Disk: `drivers/ata.c` uses bus-master DMA when a test read matches PIO at
  boot (look for "ata: hdX: bus-master DMA"). Concern: on real hardware we rely
  on the BIOS to have set a DMA mode and PIIX timings. Never add wbinvd, port
  spins, or PIO to a hot path: `-icount` cannot see their cost.
- Next: DHCP (AF_PACKET, SIOCGIF*/SIOCSIF* ioctls, broadcast).

## Step 10, the boundary (10a)

What is in the tree:

- `include/skbuff.h`, `net/skbuff.c` — one descriptor and one byte buffer.
  `alloc_skb`, `skb_reserve`, `skb_put`, `skb_pull`, `skb_push`, `kfree_skb`.
- `include/netdevice.h`, `net/netdevice.c` — `net_device`, `net_device_ops`
  (`ndo_open`, `ndo_stop`, `ndo_start_xmit`), `dev_queue_xmit`, `netif_rx`,
  `net_rx_action`, `eth_type_trans`, `dev_add_pack`. Loopback device `lo`.
- NAPI: `netif_napi_add`, `napi_schedule`, `napi_complete_done`. A poll that
  returns less than its budget is not run again until the next schedule.
- `include/pci.h`, `drivers/pci.c` — bus 0 via ports 0xCF8 and 0xCFC.
- QEMU flags now include `-netdev user,id=net0 -device e1000,netdev=net0`.
  The scan prints `pci: 00:03.0 8086:100e class 020000 irq 11`. That is
  QEMU's 82540EM. IRQ 11, BAR0 is the MMIO register block (see `pci_dev.bar`).

Concerns:

- Do not copy Linux's e1000 (or e100, 8139too, r8169) until Rodger picks a
  licence. Those files are GPLv2. The callbacks are named the way that driver
  calls them so the port stays small once the licence exists. Until then the
  next driver is ours, against this same `net_device`.
- `netif_rx` queues at most 32 packets and drops the rest. It does not run
  protocol code. `net_rx_action` does, from the main loop, so an interrupt
  handler stays short.
- One protocol handler owns the skb. A second `dev_add_pack` for the same type
  never sees it. Fine until IP and a tap both want the frame.
- Sockets still return ENOTSOCK. The card itself does answer an ICMP echo;
  see the 10b note at the top. QEMU's e1000 drops receives for one second
  after RCTL is written (`flush_queue_timer`). The first transmit waits out
  the rest of that second (`rx_ready_at` in `drivers/e1000.c`). That wait
  also hits real hardware if the first frame is sent that soon.


## Will step 9 get networking up?

No. Step 9 is disks and file systems. Nothing here opens a socket or talks to a
card. The network is step 10 (e1000, then ping).

## Intent: the network stack should look like Linux

Rodger, 2026-10-03: the kernel network stack should be as similar to Linux as we
can make it, so a later port of a network driver is small.

- The part that makes a Linux driver drop in is the boundary the driver already
  calls: `net_device`, `sk_buff`, NAPI, PCI/DMA/IRQ. The first e1000 is ours
  until a licence is chosen (DESIGN.md, later on 2026-10-03). Keep the calls.
- He also wants the stack above that boundary to follow Linux's shape (socket
  layer, `sk_buff` ownership, the same driver callbacks), not a foreign stack
  with an adapter glued on. That closes the old "lwIP or ours, either is fine"
  line. Recorded in DESIGN.md as decided 2026-10-03. The first slice (10a, above)
  is in the tree. ARP and ICMP are in (`net/ipv4.c`). Sockets are not.
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

### 9d disk speed: page cache v2, AHCI, NVMe, virtio-blk — done

Rodger asked for disk I/O "WAY faster than Linux", measured on all four disk
types. Numbers in `docs/performance.md` ("Disk happy path"): 2.0-5.6× fewer
instructions than Linux 6.1 on every operation and every type.

- The 9c-2 concern above is fixed. `fs/pcache.c` is rewritten: 4-way
  set-associative (clock bit), a quarter of RAM up to 64 MiB, table from
  `pcache_init()` at boot. Pages are tagged by a per-inode `pc_id`, not the
  inode pointer; `pcache_invalidate` just clears `pc_id` (O(1)) and the old
  pages never match again. `read()` of ext2 is `pcache_read_user` — no fs lock,
  no `prefault`.
- Misses call the new `fs_ops.readpages` (`ext2_readpages`): `bmap_run` maps up
  to 64 blocks per indirect-block lookup, runs of consecutive disk blocks become
  one `blockdev_readv` straight into the frames. Read-ahead window per inode
  (`ra_next`, `ra_win`): 4 pages, doubling to 32 while the file is read in order.
- Concern (correctness): a data block changed through the block cache may not be
  on the disk yet, so `ext2_readpages` copies any block the block cache holds
  (`bpeek`) instead of reading the disk. Keep that if you touch it. A write
  racing a fill is caught by `pc_id` changing; the fill is thrown away.
- `struct blockdev` has `.rwv` (scatter-gather read/write of `struct bio_vec`
  physical pieces, ≤ 64 pieces, ≤ 256 sectors). All four drivers implement it.
  `blockdev_map` turns a kernel buffer into pieces for drivers.
- New drivers, written from the specs (no Linux code): `drivers/ahci.c` (sda..,
  BAR 5, slot 0 only), `drivers/nvme.c` (nvme0n1, one I/O queue pair, INTx with
  INTMS/INTMC masking, PRP lists, bounce pages for 1 KiB pieces),
  `drivers/virtio_blk.c` (vda, legacy I/O-port interface). `pci_map_mmio` in
  `drivers/pci.c` maps BARs uncached at 0xF4100000+.
- IRQ sharing: `irq_install` keeps up to 4 handlers per line; each must check its
  own device (e1000, AHCI, NVMe and virtio all land on IRQ 11 in QEMU).
- Fixed on the way: `ata_readv` filled the channel's PRD table before taking the
  channel (could corrupt another thread's DMA). `kernel/bench.c` ran diskbench
  twice (the second run's "cold" was partly cached); now once.
- Writes: `bcache.c` `write_run` sends dirty neighbours as one command (sync and
  eviction); `ext2` `alloc_block` tries `next_blk` first. Writes still don't go
  through the page cache — the next big write win (see gaps).

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
- 9d: PIIX/ICH bus-master DMA is in (PIO kept as fallback). Still to do: AHCI.
  LBA28, legacy ports 0x1F0/0x170.
- 9e write: allocate clusters, update both FAT copies, directory entries.
  Test it by writing on the stick and reading the image back with mtools,
  the way ext2 is checked with e2fsck and debugfs.
- Later, not the numbered line: ext4 read, exFAT, ext4 write (a journal).
  NTFS stays out. USB disks need a USB host controller first.
- Step 8j `/proc` is still open. It is not part of finishing step 9.

## Tests

All of these passed on 2026-10-03.

- `make test` — `Self-tests: 94 passed, 0 failed`, then `TEST OK` (e2fsck and
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
  - `net_selftest` (`kernel/net_test.c`). The serial log must contain
    `net: loopback delivered`, `pci: e1000 8086:100e`,
    `net: ping 10.0.2.2 ok`, `net: dns example.com is `, and
    `net: tcp example.com HTTP ok`. The ping is an ICMP echo to QEMU's
    gateway. The DNS line is a UDP A lookup of example.com via 10.0.2.3.
    The HTTP line is a TCP connection to that address, port 80, and a
    response starting with `HTTP/`. A ping failure also prints `e1000_debug`.
- `make test-shell` — bash 5.2 still reaches a prompt and the usual shell
  checks pass. Programs now start by mapping the file.
- `tools/test-disks.sh` — the `make test` kernel with the ext2 test disk on
  AHCI, NVMe and virtio-blk; each must mount /data and pass `ext2test`,
  `fstest /data/fst` and `Self-tests: N passed, 0 failed`. Prints `ahci: OK`,
  `nvme: OK`, `virtio: OK`. (`make test` covers IDE.) `pcache_selftest` now
  checks "first touch reads a read-ahead window" (4 pages) instead of 2.
- `tools/diskbench.sh ide|ahci|nvme|virtio [myos|linux|both]` — the disk
  benchmark; `NOMAKE=1` skips the build so the four can run in parallel.
- `make bench` — numbers above, written into `docs/performance.md`.

## Other concerns

- The boot picture holds about two seconds in stage 2 (`hold` in
  `arch/i386/boot/stage2.asm`). `make test` waits that out. A Desktop trial
  UTM was never built.
- Do not commit unless Rodger asks. This work is uncommitted.
