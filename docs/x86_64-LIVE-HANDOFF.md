# x86_64 live userland — handoff for Claude (2026-10-05)

Cursor rebuilt the 64-bit live disk **userland and images** without kernel changes.
Use this note to continue **Phase F** (live desktop on the x86_64 kernel).

## What was rebuilt

| Artifact | Path | Size (approx.) |
|----------|------|----------------|
| Alpine lock (refreshed) | `live/packages.x86_64.lock` | 563 packages, SHA256 `19ad2414…` |
| Sysroot | `build/live/sysroot-x86_64/` | 6362 files |
| MyOS extras | `build/live/extra.x86_64/` | login, menu, settings, files, preview, xsettings, theme, WritePerfect |
| Full rootfs tar | `build/live/rootfs-x86_64.tar` | ~1260 MiB unpacked (Chromium, not Firefox) |
| Login initrd | `build/live/login-initrd-x86_64.cpio` | ~120 MiB |
| ext2 myos-root | `build/x86_64/myos-live-root.img` | 2048 MiB |
| UEFI live stick image | `build/x86_64/myos-live-uefi.img` | ~2.15 GiB (152 MiB ESP + root) |
| RAM login image | `build/x86_64/myos-uefi-login.img` | (from `make ARCH=x86_64 live-login-initrd` + kernel) |

**Commands used:**

```bash
python3 tools/fetch-live.py --arch x86_64 --update   # stale gcc apk in lock; re-resolve
make ARCH=x86_64 build/live/sysroot-x86_64/.done
make ARCH=x86_64 live-extra
make ARCH=x86_64 live-login-initrd live-hd live-uefi
```

**ELF64 musl binaries** (examples):

- `build/live/extra.x86_64/usr/bin/myos-login` — `ELF 64-bit`, interpreter `/lib/ld-musl-x86_64.so.1`

Build logs: `build/fetch-live-update.log`, `build/live-extra.log`, `build/live-images.log`.

## Boot test results (2026-10-05)

| Test | Result | Notes |
|------|--------|-------|
| `make ARCH=x86_64 test-uefi-login` | **PASS** (Makefile grep) | Runit starts; **no myos-login yet** |
| `make ARCH=x86_64 test-live-uefi` | **PASS** | `ext2: sda2 mounted at /` on 2048 MiB myos-root |
| Graphical myos-login | **Not reached** | See serial tail below |

### Serial log tail (both login and live UEFI paths)

```
initramfs: 6747 files and directories
drm: bochs 1234:1111, 16 MiB, 1920x1200
fbcon: 120x37
ext2: sda2 mounted at /: ...          # live-uefi only
net: lo up
- runit: enter stage: /etc/runit/1
syscall 430 not implemented (mount)
syscall 326 not implemented (install)
syscall 190 not implemented (install)
First boot: finishing the install (log: /var/log/first-boot.log)
```

The parenthetical names are **thread names** (`kernel/syscall.c` logs `thread_current()->name`), not syscall names.

Linux x86_64 numbers (for implementers):

| NR | Typical name | Who logged it |
|----|----------------|---------------|
| 430 | `fsopen` (util-linux `mount` on Alpine 3.20) | thread `mount` |
| 326 | `copy_file_range` | thread `install` (first-boot / apk scripts) |
| 190 | `vfork` | thread `install` |

Stage 1 is [`live/overlay/etc/runit/1`](../live/overlay/etc/runit/1) (proc/sys/tmpfs mounts). First-boot is [`live/overlay/etc/myos/first-boot.sh`](../live/overlay/etc/myos/first-boot.sh). Neither completes cleanly until those syscalls exist (or userland is rewritten to avoid them — **not recommended**; mount/fsopen is the real fix).

**Screenshot:** `build/x86_64/live-login-screenshot.png` — kernel fbcon text after boot stall (QEMU window may still say “Guest has not initialized the display” until DISPI/Xorg; fbcon draws to Bochs VRAM).

## Suggested next kernel steps (priority)

1. **Implement 430 / fsmount / fsconfig / fspick** (or make legacy `mount(2)` path work for proc/sys/tmpfs) so `/etc/runit/1` can mount `/proc`, `/sys`, `/run`, `/dev/shm`.
2. **`vfork` (190)** — used by Alpine shell/scripts during first-boot; can stub to `fork` initially if CLONE flags match vfork semantics.
3. **`copy_file_range` (326)** — first-boot install scripts and coreutils; needed early in bring-up.
4. Then re-run `make ARCH=x86_64 test-uefi-login` and confirm serial shows **`myos-login`** or X starting (grep in Makefile ~677).
5. **Phase F continued:** dynamic musl programs (Xorg, picom, PAM), MIT-SHM ([`docs/gaps.md`](gaps.md)), mmap of PCI BAR / GOP for X ([`docs/x86_64-PORT-PLAN.md`](x86_64-PORT-PLAN.md)).
6. **USB MSC** still blocks “stick only” hardware ([`docs/gaps.md`](gaps.md)); QEMU live image uses **AHCI** so ext2 root works.

## Userland left TODO (no kernel required)

- None blocking **build** — pipeline is green after lock refresh.
- Optional: commit updated `live/packages.x86_64.lock` if Rodger wants the refreshed Alpine pins in git.
- Optional: `live/overlay.x86_64/` only if x64-specific config diverges from [`live/overlay/`](live/overlay/) (not needed yet).

## i386 regression

Not re-run in this session; Pentium path unchanged by live-only work.
