# Performance suggestions: making MyOS faster without losing anything

Status: **suggestions** (2026-10-12), each grounded in a measurement or in
the code as it is today. Measured results, before and after each change,
go to `docs/performance.md` (CLAUDE.md, rule 3). Done or decided so far
(2026-10-15):
- **G1, MIT-SHM on:** done (2.3–2.5× faster transfers, 6× for small pictures).
- **B6, faster `exec`:** programs map the page cache's pages of their files
  instead of copying them, with fault-around (a GTK program starts 5.4×
  faster, the idle desktop uses ~115 MB less). Read-ahead was there already.
- **C5, the login screen's cached picture:** not worth it. Its own work is
  ~240 ms of CPU in QEMU; the rest of its time it waits for the processor
  (C3 is what would help).
- **D1, the gap after icewm starts:** starting the programs alongside IceWM
  made the desktop appear later on one processor (tried, reverted). It is
  work, not waiting; B6 shortened it.
- **The idle desktop's wake-ups:** not in this list, but done: 3.9% → 1.4%
  CPU, no programs started in the background.
- **A5, the 32-bit loader's 2 s pause:** removed (2026-10-17): 2 s off every
  old PC's boot.
- **B1, DHCP in the background:** done (2026-10-17): 1 s off every boot in
  QEMU, 1-3 s on a real network, up to 15 s on one that doesn't answer.
- **C1, the first boot at build time:** done for the UEFI live disk
  (2026-10-17): its login screen is up at 8.6 s in QEMU instead of 15.7 s
  (`live/tools/live-prepare.py`). The BIOS live disk still does it at first boot.
- **A1, an installed system without the fallback:** done (2026-10-17): the
  loader's step goes from 1.46 s to 0.01 s in QEMU, and 240 MB of RAM stay
  free. A2 (the stick deciding for itself) not yet: see docs/performance.md.

The rule for everything below: **no capability is given up**. Every
fallback, driver, service and program still exists; things are only done:
- **once** instead of every time;
- **in the background** instead of in the way;
- **only when they are needed**;
- **with fewer copies**.

- **Part 1 — Boot speed:** from power-on to the desktop.
- **Part 2 — Graphics and the window server:** how pixels get to the
  screen, a Mac-like design where X11 is a guest, and Mission Control.

## Part 1 — Boot speed

Boot time is one of the places where CLAUDE.md says a small kernel should
beat Linux.

### 1.1 Where the time goes today

Measured with `tools/boottime.py` on the 64-bit live image, in QEMU on the
Mac (a UTM-like machine: q35, AHCI disk, e1000). Like every QEMU number
these compare versions of MyOS; real hardware is in §1.2.

**An everyday boot** (first-boot work already done):

| Time (s) | Stage | Took | What |
|---:|---|---:|---|
| 0 → 2.3 | QEMU's firmware (OVMF) | 2.3 | not ours |
| 2.3 → 3.9 | our UEFI loader | **1.5** | reads `KERNEL.BIN` (1 MB) and `INITRD.CPIO` (**240 MB**). With a 1 MB initramfs this step takes **0.02 s** |
| 3.9 → 4.9 | the kernel, steps 2–15 | 1.0 | **1.0 s waiting for a DHCP lease** (step 13); everything else ~50 ms |
| 5.0 → 6.3 | runit stage 1 | **1.3** | a dozen small commands (mounts, `mkdir`, `ln`, `install`, `hostname`); one program start costs **~24 ms** in QEMU |
| 6.3 → 6.7 | runit stage 2 → `myos-login` | 0.4 | services start together |
| 6.7 → 10.2 | X server + login screen | **3.5** | X: 1.35 s, of which **0.65 s loads Mesa's software OpenGL** (`swrast`, for GLX); then the login screen decodes and scales its background picture |
| **10.2** | **login screen** | | |
| +0.2 | icewm running | | |
| +3.1 … +3.9 | Start menu, Wi-Fi icon, desktop icons | **3–4** | a 3 s gap after icewm starts, then four GTK programs start at once |

**The first boot of a fresh stick or install** adds **14.3 s** in runit
stage 1. That is `first-boot.sh` running Alpine's package scripts:
font cache, icon caches, MIME database, GLib schemas and so on. The login
screen then appears at 24.8 s.

**The 32-bit loader** (`arch/i386/boot/stage2.asm`) still calls `hold`:
**2 s with the picture on screen after loading**, at every boot of every
old PC. It was added so the picture could be seen in QEMU.

### 1.2 On the real machines

QEMU makes the CPU slow and the disks instant; real machines are the other
way round. Things QEMU hides:

| Machine | What dominates | Ours? |
|---|---|---|
| **MSI, booted from a USB stick** | the firmware's POST (several seconds); the firmware reading the **240 MB initramfs** through its own USB driver: at 30–150 MB/s that's **2–8 s** | the initramfs, yes |
| **MSI or Pocket 4, installed on NVMe** | POST; then the same 240 MB, which an installed system never uses (the installer copies the whole stick's EFI partition); then waiting for **every USB device**, though the root is on NVMe (§1.4, B2) | yes, both |
| **Old PC, BIOS** | POST (5–10 s); stage 2 reading ~3 MB through the BIOS (0.1–0.3 s from IDE, **1–5 s from USB** on a slow BIOS); **the 2 s hold**; an IDE channel with no drive (up to `PROBE_TIMEOUT`, 200 ms, per channel) | all but POST |
| **Every machine on a network** | the DHCP wait: 1 s in QEMU, 1–3 s on a real network | yes |

The first job is therefore a boot clock **on the real machines** (§1.3).
Each item below is then measured there before and after, and the numbers
go to `docs/performance.md`.

### 1.3 Measure first

- **`tools/boottime.py`** (new): the timeline above, for any image, in
  QEMU. `--everyday` times the second boot, `--no-login` stops at the
  login screen.
- **A boot clock in the kernel.** Each `boot_step()` line (and `dmesg`)
  gets the milliseconds since the loader started. The UEFI loader passes
  its own start time (TSC) in `bootinfo`, so firmware → loader → kernel
  are all on one clock. `/proc/myos/boottime` lists the steps with their
  times, readable over ssh from the MSI.
- **User space on the same clock.** runit stage 1, each service's start,
  `myos-login`'s "X is up" and "screen is up", and the session's programs
  write timestamps to the log. X's own log already has them.
- **`make bench`** keeps guarding the kernel's own CPU work (today a few
  milliseconds), so none of this grows it.

### 1.4 The plan

#### A. Firmware and loader

**A1. An installed system doesn't carry the live stick's fallback.**
- **What it is:** the 240 MB `INITRD.CPIO` (the login system, in RAM) is
  there so that a machine whose disk MyOS **can't** drive still reaches
  the login screen from the stick.
- **Why installed systems don't need it:** the installer only installs
  onto a disk MyOS can drive, so an installed system always finds its
  root.
- **The change:** the installer copies the kernel, the loader and a small
  initramfs only. `myos-mkesp` already writes whatever folder it is
  given.
- **Saving:** 1.4 s in QEMU; 0.5–2 s on NVMe, every boot. The ESP also
  shrinks from 512 MiB to under 64 MiB.
- **Lost:** nothing. The stick still has the fallback.

**A2. The stick loads the fallback only when the kernel can't find its
disk without it.**
- **What the loader can see:** it is still inside UEFI's boot services,
  so it knows the device it was started from. Its device path says what
  kind of controller that is (USB, NVMe, SATA), and the PCI I/O protocol
  says which USB controller (xHCI, the one MyOS drives).
- **The rule:** if MyOS has a driver for that path, and the same disk has
  a `myos-root` partition (the loader reads the GPT through BlockIo), the
  kernel will find its root. The loader loads a **small** initramfs.
  Otherwise it loads the full one, as today.
- **Saving:** 2–8 s per boot on a USB stick on real hardware (1.4 s in
  QEMU).
- **Lost:** nothing. The fallback is still loaded wherever it is needed,
  and a boot-menu choice ("start from RAM") can force it.

**A3. Compress the fallback.** Where the 240 MB still has to be read
(A2's fallback case), an LZ4-compressed archive is roughly half the
bytes.
- LZ4 decompresses at memory speed, about 0.5 s for 240 MB on a Pentium
  4 class machine, much less on the MSI.
- The kernel unpacks it into the pages the initramfs uses anyway, so
  files are still read straight from them afterwards: CLAUDE.md, rule 5
  ("zero-copy").
- Worth it only where reading is slower than decompressing: USB sticks
  through firmware drivers. Measure first.

**A4. A boot entry, written by the loader.**
- **Why:** today the firmware finds MyOS through the fallback path
  `EFI/BOOT/BOOTX64.EFI`, after scanning, or after F11.
- **The change:** on its first start from a fixed disk, the loader writes
  a `Boot####` variable for itself and puts it first in `BootOrder`
  (`SetVariable` works in boot services).
- **Saving:** the firmware's device scan, 0.5–3 s depending on the board.
  It also fixes "the MSI started the wrong system".
- **Lost:** nothing. The fallback path stays.

**A5. The old PCs' loader.**
- Remove the **2 s hold**, or make it a minimum: hold only until 2 s
  after the picture appeared, so a slow load waits no longer. The picture
  stays on screen until the kernel draws anyway.
- Read the kernel and initramfs with the largest BIOS reads the BIOS
  allows (extended INT 13h, up to 127 sectors a call).
- Then measure on the Pentium 4 whether LZ4 (A3) pays off for stage 2's
  3 MB too.
- **Saving:** 2 s on every old PC boot, plus whatever the bigger reads
  give from USB.

#### B. The kernel

**B1. DHCP in the background.**
- **Today:** step 13 waits for the lease.
- **The change:** send the request and go on. The lease arrives while
  runit and X start.
- **Why that is safe:** programs already get the DNS server live
  (`/etc/resolv.conf` → `/proc/net/dns`), and a program that needs the
  network waits for it as it would for a slow network.
- **Saving:** 1.0 s in QEMU, 1–3 s on real networks, and the whole
  timeout on a network that never answers.

**B2. Wait for USB only when the root is on USB.**
- **Today:** `usb_wait_boot(8000)` waits for the first pass over every
  USB port on any machine with an xHCI controller.
- **The change:** wait only when the loader says MyOS started from USB
  (`BOOT_FROM_USB`), or when no root has been found yet. Elsewhere USB
  devices keep arriving while the boot goes on.
- **Saving:** USB enumeration (port debounce and reset, about 0.1–0.3 s
  per device) on every installed machine.
- **Needs:** devices that appear after X has started (USB keyboards and
  mice, B50) need hot-plug in `evdev`. Until then those stay in the wait.

**B3. Start the slow hardware first, and side by side.**
- Start the USB controller's enumeration right after PCI, before
  graphics and sound, so it overlaps with them.
- Give each disk controller's reset and identify its own thread. iwlwifi
  already loads its firmware in the background.
- **Saving:** the longest device instead of the sum of all.

**B4. Find the real waits on real hardware**, with the boot clock:
- an IDE channel with no drive (`PROBE_TIMEOUT` 200 ms; `SPIN_LIMIT` ~1 s
  on a floating bus);
- AHCI ports (`wait_clear` up to 1 s each);
- the NVMe controller's ready time (`CAP.TO`);
- xHCI port debounce.

Fix what shows up, for example by skipping ports and channels the
controller says are empty (AHCI's `PxSSTS`, an IDE bus that reads
`0xFF`).

**B5. The kernel mounts the file systems every program expects.**
- **The change:** `/proc`, `/sys`, `/run` and `/dev/shm` are mounted by
  the kernel before `init`, as Linux does for `/dev` with devtmpfs.
- **Why it helps:** stage 1's `mountpoint`/`mount` pairs become no-ops.
  util-linux's `mount` also tries the new mount API first
  (`syscall 430 not implemented`, then a fallback).
- **Saving:** about 8 program starts, 0.2–0.4 s in QEMU.
- **Lost:** nothing. Stage 1 keeps its lines; they find everything
  already mounted.

**B6. Faster `exec`.**
- **Today:** a program start costs ~24 ms in QEMU (`/bin/true`, dynamic
  musl). Every boot stage is made of them, so this multiplies through
  stage 1, the services, X's helpers and the session.
- **What to do:** measure what dominates with `make SYSTRACE=1` and `make
  bench`. Likely costs:
  - page faults on the program and `libc.so`;
  - one page read ahead at a time;
  - copying instead of mapping.
- **Likely fixes:**
  - map shared libraries straight from the page cache;
  - **fault-around**: map up to 16 already-cached pages per fault;
  - read ahead more than one page.
- The before/after numbers go into `docs/performance.md`.

#### C. User space, before the login screen

**C1. The first boot happens when the image is built.**
- **What it is:** the 14 s of `first-boot.sh` (Alpine's install scripts)
  only make caches and databases.
- **The change:** the image build boots the new image once in QEMU,
  headless, lets `first-boot.sh` run, and powers off. The image ships
  with the work done: the same scripts, the same results, on the build
  machine.
- **Saving:** 14 s on the first boot of every stick and every install;
  on a Pentium 4, likely minutes.
- **Lost:** nothing. If someone deletes `first-boot.done`, the scripts
  still run at boot as today.

**C2. Stage 1 in fewer programs.** After B5, the rest of stage 1 is
`mkdir`, `chmod`, `ln`, `install`, the `supervise` loop and `hostname`.
- **Option:** bash builtins, plus one small static program for the rest.
- **Saving:** most of the remaining ~1 s in QEMU.
- **Lost:** nothing. The script stays readable; it calls one helper.

**C3. The login screen first.**
- **Today:** runit starts every service together, so the X server
  competes with `sshd`, `wpa_supplicant` and the rest.
- **The change:** give `myos-login` and X the processor first, through
  the OS class in docs/TIME-SLOTS.md. Services that nobody is waiting
  for start a moment later.
- **Saving:** the login screen arrives as if it were alone. Nothing
  starts later than people notice.

**C4. The X server.**
- **0.65 s of its 1.35 s** load Mesa's software OpenGL (`swrast`) for
  GLX. GLX stays, because OpenGL programs and WebGL need it. B6's faster
  mapping of a large library is the fix to measure first.
- **Check the keyboard map:** X compiles it at each start (`xkbcomp`)
  unless it can cache the result in `/var/lib/xkb`. Make sure it can.

**C5. The login screen's picture, cached.**
- **Today:** `myos-login` decodes and scales the photograph at every
  start.
- **The change:** keep the finished screen, background, bands, logo and
  fixed text, for each screen size, in `/var/cache/myos-login/WxH`. The
  next start maps it and sends it to X: no decoding.
- **Lost:** nothing. A new theme or screen size simply rebuilds the
  cache.

#### D. After logging in

**D1. The 3 s gap after icewm starts.** The Start menu and the desktop
icons appear about 3 s after icewm. Find out with the boot clock whether
that is icewm-session's order (its `startup` script runs after icewm is
fully up), icewm's own start, or the GTK programs' start-up.
- **Fix:** start what you see first: the desktop and the taskbar's Start
  menu first, the tray icons a moment later.

**D2. Fewer separate GTK start-ups.** `myos-menu`, `myos-wifi`,
`myos-desktop`, `myos-trash`, `myos-volume` and `myos-xsettings` each
pay GTK's start-up: linking about 50 libraries, then reading the theme
and icon caches.
- **First:** B6 (cheaper `exec`), and checking the caches are all built
  at image time (C1).
- **Then, if it still matters:** one desktop-shell process could hold
  the desktop, the Recycle can and the tray icons, as one GTK start
  instead of four. That's a bigger change; do it only if the numbers
  call for it.

#### E. Bigger ideas, later

- **E1. A boot read-ahead profile.**
  - **What it does:** the kernel records which file blocks the boot
    reads, in order. Next time it reads them at the start in a few large
    requests.
  - **Why it helps:** a USB stick or a hard disk serves large sequential
    reads many times faster than scattered 4 KiB pages (CLAUDE.md
    names read-ahead in step 9).
  - **Lost:** nothing. A missing or stale profile only means reading
    as today.
- **E2. Lay the boot's files out in boot order** when the image is built
  (`live/tools/mklivehd.sh`), so those reads are sequential even without a
  profile.
- **E3. Sleep instead of shutting down.** `s2idle` or S3
  (docs/power-performance-settings.md): back at the desktop in about a
  second. That isn't boot speed, but it is the fastest start there is.

### 1.5 What it adds up to

In QEMU, everyday boot, to the login screen:

| | Today | After A1/A2, B1, B5, C2, C5 |
|---|---:|---:|
| firmware (not ours) | 2.3 s | 2.3 s |
| loader | 1.5 s | ~0.05 s |
| kernel | 1.0 s | ~0.05 s |
| stage 1 | 1.3 s | ~0.2 s |
| X + login screen | 3.5 s | ~2.5 s, before C4 and B6 |
| **login screen** | **10.2 s** | **~5 s** |
| first boot of a fresh image | 24.8 s | the same ~5 s (C1) |

These are estimates until each step is measured. On real hardware the
savings are larger, because the loader's 240 MB read and the network wait
take longer there than in QEMU. On old PCs the 2 s hold (A5) comes off
every boot.

### 1.6 Steps

Each step comes with a before/after number in `docs/performance.md`:
QEMU via `tools/boottime.py`, and the MSI over ssh with the boot clock.
The cheap and large ones come first.

1. **The boot clock** (§1.3): kernel steps, `/proc/myos/boottime`, and the
   user-space timestamps.
2. **B1: DHCP in the background.**
3. **A5: the 32-bit loader's 2 s hold.**
4. **C1: first boot at build time.**
5. **A1: the installer copies a small initramfs.** Then **A2:** the
   stick's loader decides.
6. **B2: wait for USB only when the root is on USB.** Then **B3:** start
   USB first.
7. **B5 + C2: a short stage 1.**
8. **C5: the login screen's cache.** Then **C4:** X's keyboard map and
   GLX.
9. **D1 + D2: the session.**
10. **B6: faster `exec`.** It helps everything.
11. **A4: the boot entry.**
12. **A3, E1, E2: compression and read-ahead,** where the measurements
    show the disk is the limit.

## Part 2 — Graphics and the window server

### 2.1 How a pixel reaches the screen today

| Machine | Kernel driver | What `/dev/fb0` is |
|---|---|---|
| QEMU / UTM | Bochs DRM (`drivers/gpu/drm/tiny/bochs.c`) with `drm_fbdev.c` | the card's video memory, mapped write-combining |
| MSI, Pocket 4 (64-bit kernel) | `drivers/video/simplefb.c` | the firmware's (GOP) framebuffer, mapped write-combining: **no copy** |
| 32-bit kernel, GOP framebuffer above 4 GiB | `simplefb.c` with a RAM shadow | a RAM copy; the `fbblit` thread copies **the whole screen every 16 ms** (`fb_shadow_flush`, 8 MB at 1920×1080: ~500 MB/s, changed or not) |

On top of it the X server runs the `fbdev` driver (`10-myos.conf`).
Programs draw with cairo, GTK or Imlib2 in their own memory, and every
picture then travels like this:

```
program's buffer ──XPutImage──▶ socket ──▶ X server's copy ──▶ X's shadow ──changed areas──▶ /dev/fb0
                   (copy 1)       (copy 2)                       (copy 3)                     (copy 4)
       with the compositor (picom, xrender): + the window read back and composited again (copies 5, 6)
```

What this costs, and why:

- **MIT-SHM is switched off** in
  `live/overlay/etc/X11/xorg.conf.d/10-myos.conf`, "because MyOS has no
  System V shared memory". **That reason is out of date.** `kernel/shm.c`
  has `shmget`, `shmat`, `shmdt` and `shmctl` (`IPC_STAT`, `IPC_RMID`,
  `SHM_RDONLY`). X's ownership check has `SO_PEERCRED` (`net/unix.c`).
  `memfd_create` with seals is there for the newer fd-passing form
  (`kernel/sys_fs.c`, `kernel/syscall.c`). So every icon, photo, web page
  and video frame is copied twice through a socket when it could be
  shared.
- **There is no GPU driver.** All drawing is done by the CPU, OpenGL
  included: Mesa's `swrast`, whose loading is 0.65 s of the X server's
  1.35 s start (Part 1, §1.1). The graphics card only displays the
  finished picture.
- **No `/dev/dri` for programs.** The DRM core (`drm_core.c`) serves only
  the kernel's own `/dev/fb0`. So no modern display server (Wayland), and
  no X `modesetting` driver.

### 2.2 The answer to "a Mac-like design, X11 an afterthought"

**The Mac:** each app draws into its own buffer in shared memory (IOSurface).
WindowServer, the one process that owns the screen, composites those
buffers, on the GPU, and copies only what changed. X11 programs run in
XQuartz, a guest X server that turns each X window into a Mac window.

**On Linux that design is Wayland:**
- each program draws into a buffer it shares with the compositor (a
  `memfd`, or GPU memory);
- the **compositor** is the window server, and puts the buffers on the
  screen with damage tracking;
- **XWayland** is the XQuartz: a guest X server that turns each X11
  window into a Wayland window.

**What breaks?** With XWayland, almost nothing:

| | Programs |
|---|---|
| **Native Wayland, the fast path** | GTK3/GTK4 programs (AbiWord, Gnumeric, our `myos-*` programs once ported), Firefox, Chromium, Electron (VS Code), LibreOffice, Qt5/Qt6, mpv, SDL2 games |
| **Through XWayland, unchanged** | xterm, **xeyes, xfig**, Motif/Xt programs, Tk (Python's `tkinter`), Java Swing/AWT (until OpenJDK's Wayland port lands), Wine, Emacs built for X |
| **Changes: they act on *other* programs' windows or the whole screen** | `xdotool`, `xwd`/`import` screenshots, `xbindkeys`, X window managers (IceWM) and compositors (picom). Wayland does these in the compositor, or through portals |
| **Ours, to port** | `myos-menu`, `myos-desktop`, `myos-wifi`, `myos-volume`, `myos-trash`, `myos-xsettings`, `myos-login` (§2.5) |

**Does "directly with the graphics card" make it much faster?** Only half
of it does:
- **What Wayland alone gives:** fewer copies, no X round trips, and only
  changed areas redrawn. Even with all the drawing on the CPU, that is a
  clear gain, most of all on old PCs, where memory bandwidth is the limit.
- **The "way faster" part, the GPU:** compositing in the GPU, OpenGL and
  Vulkan in hardware, video decode, and lower power. That needs **GPU
  drivers** (§2.6), the largest job in this document.

### 2.3 Phase 1: fewer copies in today's X11 (days)

**G1. MIT-SHM on.** Expected to be the largest win for the least work.
- **Change:** delete the `Section "Extensions"` block (`Option "MIT-SHM"
  "Disable"`) and its comment from
  `live/overlay/etc/X11/xorg.conf.d/10-myos.conf`.
- **Kernel, to confirm with a test first** (`user/sys/systest.c`, a
  `t_shm_xlike` case doing what X and a client do):
  - the client creates a segment (`shmget(IPC_PRIVATE, size, IPC_CREAT | 0600)`);
  - the client attaches it, then the server attaches it **by id**,
    read-only (`SHM_RDONLY`);
  - `IPC_STAT` reports the creator's uid and the mode, which X compares
    with `SO_PEERCRED`;
  - the client marks it `IPC_RMID` while both are still attached. It must
    stay alive until the last detach, then free its pages;
  - after a `fork` the attach counts stay right, and `exec` detaches.
- **The newer path** (MIT-SHM 1.2, `xcb_shm_attach_fd`): a `memfd` sent
  with `SCM_RIGHTS`, which both are already in. Test that cairo's and
  GTK's xcb-shm uses work.
- **Check on screen:** icons, the login photo (Imlib2), GTK pictures,
  Firefox pages. The old comment says what failure looked like: "draw
  nothing".
- **Measure:**
  - add `x11perf` (Alpine) to the build tools: `x11perf -putimage500`
    against `-shmput500`;
  - the login screen's time to "up" (Part 1, C5);
  - one 1920×1200 picture sent to X with `XPutImage` against
    `XShmPutImage`.

**G2. The compositor, decided by the numbers.**
- picom's `xrender` backend redoes in the CPU every window that changes:
  one or two more copies per change.
- The installer already measures it (`compositor_ms` in the install
  plan), and Settings writes `compositor=` to `/etc/myos/desktop.conf`.
- **Change:** with no GPU driver, the default becomes `compositor=none`
  when the measured redraw costs more than about 4 ms (a quarter of a
  60 Hz frame). Shadows and fades stay available as a choice in Settings ›
  Displays.
- **Measure:** idle CPU and the time of a full-screen window move with
  and without picom.

**G3. X's start, and GLX.**
- Measure what the 0.65 s for `swrast` is made of (`make SYSTRACE=1`:
  the `mmap`s and page faults of a ~20 MB `.so`).
- Fix it in the kernel, for every big library: fault-around and
  read-ahead (Part 1, B6), not by dropping GLX, which OpenGL programs and
  WebGL use.
- Confirm X caches its compiled keyboard map in `/var/lib/xkb` (Part 1,
  C4).

**G4. The 32-bit kernel's shadow: copy only what changed.** This is
`simplefb.c` with a framebuffer above 4 GiB.
- **The change:** **deferred I/O**, as Linux's `fb_deferred_io` does it:
  - `fb_mmap` (`drivers/video/fbmem.c`) maps the shadow **read-only**;
  - the first write to each page faults (`mm/mm.c` calls a new
    `fb_page_mkwrite`), which marks the page dirty in a bitmap, makes it
    writable and wakes `fbblit`;
  - `shadow_sync` copies only the dirty pages, makes them read-only
    again, and sleeps when nothing is dirty. No fixed 16 ms wake-up.
- **Saving:** from ~500 MB/s always, to nothing when idle and only the
  changed rows when not. Each page costs one fault per frame.
- **Measure:** `fbblit`'s CPU time while idle and while typing.

### 2.4 Phase 2 groundwork: a real display device, `/dev/dri/card0` (weeks)

Every modern display server needs the kernel's mode-setting API (KMS).
X's `modesetting` driver can use it too, so this pays off even before
Wayland.

**The kernel's user API** (new `drivers/gpu/drm/drm_ioctl.c` on top of
`drm_core.c`; Linux `include/uapi/drm/drm.h` and `drm_mode.h` give the
numbers and structs):

| Group | ioctls |
|---|---|
| Who and what | `DRM_IOCTL_VERSION`, `GET_CAP` (`DUMB_BUFFER`, `CRTC_IN_VBLANK_EVENT`, `TIMESTAMP_MONOTONIC`), `SET_CLIENT_CAP` (`UNIVERSAL_PLANES`, `ATOMIC`), `SET_MASTER` / `DROP_MASTER` |
| The screen | `MODE_GETRESOURCES`, `GETCONNECTOR`, `GETENCODER`, `GETCRTC`, `SETCRTC`, `GETPLANERESOURCES`, `GETPLANE`, `GETPROPERTY`, `OBJ_GETPROPERTIES` |
| Buffers | `MODE_CREATE_DUMB`, `MAP_DUMB` (then `mmap`), `DESTROY_DUMB`, `ADDFB2`, `RMFB` |
| Showing them | `MODE_PAGE_FLIP` with a `DRM_EVENT_FLIP_COMPLETE` read from the fd; `MODE_ATOMIC`; `MODE_DIRTYFB` (which rectangles changed) |

**Two drivers behind it:**
- **Bochs (QEMU/UTM):** real page flips, by moving the scan-out start in
  video memory. It's the Y-panning fbcon already does.
- **simpledrm (GOP: MSI, Pocket 4):** a port of Linux's
  `drivers/gpu/drm/tiny/simpledrm.c`, the way `tiny/bochs.c` was ported.
  It has one mode and no real vblank, so flip events come from a 60 Hz
  timer, as Linux's simpledrm does. `DIRTYFB` tells it what to copy.

**The rest, so that unmodified libdrm, wlroots and Mesa find it:**
- the node `/dev/dri/card0` (character device, major 226, minor 0);
- `/sys/class/drm/card0`, with its `dev` and its `device` link to the PCI
  device;
- `/sys/dev/char/226:0`.

`libudev-zero` (also in the touchpad plan) reads these instead of udev.

**Check:** `modetest` from libdrm lists the connector and mode, and draws
its test pattern. X runs with `Driver "modesetting"` and shadow + damage
instead of `fbdev`.

### 2.5 Phase 2: the window server, with X11 as a guest (weeks)

**Packages** (Alpine, unmodified, downloaded at build time):
- `wayland`, `wlroots`;
- **`labwc`**: a small stacking compositor, Openbox-style, with themes.
  It's the starting point; our own window server can be written later on
  wlroots, with the Golden Hour look;
- `xwayland`;
- `seatd` with `libseat`: a small root service that hands the display
  and input devices to the logged-in user;
- `foot`: a native terminal;
- `gtk-layer-shell`: panels and desktops for GTK programs;
- `libinput` + `libudev-zero` (shared with the touchpad plan).

**The kernel** has most of what Wayland needs already:
- Unix sockets with `SCM_RIGHTS` and `SO_PEERCRED`;
- `memfd_create` with seals;
- `epoll`, `eventfd`, `poll`.

Missing:
- `/dev/dri/card0` (§2.4);
- evdev's absolute and multitouch ioctls (in the touchpad plan; libinput
  asks them of every device);
- hot-plug notices, later.

**The session:**
- `myos-login` offers "MyOS (Wayland, preview)" next to today's X11
  session, which stays.
- For a Wayland session it starts `seatd` and runs
  `labwc -s /etc/myos/session-wayland` as the user, with
  `WLR_RENDERER=pixman` until there is a GPU driver.
- GTK, Qt, Firefox and Chromium pick Wayland by themselves
  (`XDG_SESSION_TYPE=wayland`, `GDK_BACKEND=wayland,x11`,
  `MOZ_ENABLE_WAYLAND=1`, `--ozone-platform-hint=auto`).
- Everything else runs in XWayland, started on demand.

**Porting our programs**, one by one:

| Program | Today (X11) | Under Wayland |
|---|---|---|
| taskbar and Start menu | IceWM's taskbar + `myos-menu` | a new `myos-panel` (GTK + gtk-layer-shell, top layer, anchored to the bottom). It lists windows with `wlr-foreign-toplevel-management` and shows tray icons as StatusNotifierItems (D-Bus) |
| `myos-desktop`, `myos-trash` | windows on the root window | gtk-layer-shell surfaces on the background layer |
| `myos-wifi`, `myos-volume` | XEmbed tray icons (`GtkStatusIcon`) | StatusNotifierItem over D-Bus, shown by `myos-panel` |
| `myos-xsettings` | XSETTINGS for scale, DPI and cursor | labwc's output scale + `XCURSOR_THEME`/`XCURSOR_SIZE` + `~/.config/gtk-3.0/settings.ini` |
| `myos-login` | starts Xorg, draws with Xlib | first: unchanged, then hands over to the Wayland session. Later: a native greeter |
| Force Quit (`myos-forcequit`) | `_NET_WM_PID` from the window manager | the panel's toplevel list, plus the client's pid from the compositor |
| window decorations (the theme) | IceWM theme | a labwc theme made from `live/assets/themes/golden-hour` |

**What gets faster, and how we'll know:**
- No `XPutImage`: a window's pixels stay in the `memfd` the program drew
  them into.
- No X round trips.
- wlroots copies only damaged areas to the screen. On Bochs it flips
  instead of copying.
- **Measure, for both the X11 session and the Wayland preview:**
  - time from starting a program to its first frame (GTK, Firefox);
  - frame times while moving or resizing a window;
  - idle CPU with the desktop up;
  - key-press-to-screen time (`sendkey` in QEMU, as in docs/TIME-SLOTS.md);
  - memory in use.

  The Wayland session becomes the default only when it wins on every
  one, with nothing missing.

### 2.6 Phase 3: GPU drivers (months each)

The "way faster" part: compositing, OpenGL, Vulkan and video in the GPU,
and lower power. They come through the Linux-driver shim that already
runs iwlwifi, with Mesa unmodified on top.

| Machine | Kernel driver (Linux) | Mesa / video | Size and notes |
|---|---|---|---|
| MSI: Intel Xe, gen12 | `i915` | `iris` (GL), `anv` (Vulkan), `intel-media-driver` (VA-API) | about 300 k lines. Needs Intel's GuC/HuC/DMC firmware (`request_firmware`, as iwlwifi) |
| Old PCs: Intel GMA 855/915/945 | `i915` (its oldest generations) | `crocus` / `i915` | the same driver, older paths |
| Old PCs: ATI Radeon 7000–X1950 | `radeon` | `r300`, `r600` | |
| GPD Pocket 4: Radeon 780M/890M | `amdgpu` (with its Display Core) | `radeonsi`, `radv` | about 3 M lines with register headers: the largest port in the project. It also brings the backlight and power (docs/hardwareRequirements.md) |

**The kernel infrastructure all of them need**, built once in the shim:
- GEM objects backed by `shmem` pages;
- `dma-buf` and PRIME, to share buffers between programs and the
  compositor;
- `dma_fence` and `drm_syncobj`;
- the GPU scheduler (`drm_sched`);
- TTM (`amdgpu`, `radeon`);
- runtime power management;
- no IOMMU at first (passthrough).

**Then:** wlroots switches from `pixman` to its GLES2 renderer, and the
compositor composites in the GPU. XWayland gets `glamor`, so X11
programs are accelerated too.

### 2.7 Mission Control: the feature that decides the design

Rodger's goal (2026-10-12): **Mission Control exactly like macOS.** Move
the pointer into the corner, and the whole screen zooms out:
- every window shrinks and moves, so all of them can be seen side by
  side, without overlapping;
- the desktops (**Spaces**) sit in a row along the top, each a live
  picture, with a **+** to add one.

![macOS Mission Control: the Spaces bar (Desktop 1–7 and +) along the top, the windows of this desktop spread out and shrunk below it](window-server/macos-mission-control.webp)

*macOS Mission Control, the model (Rodger's Mac, 2026-10-12): the Spaces
bar along the top, the current desktop (Desktop 7) outlined in blue, its
windows spread out below. The picture shows Rodger's own desktop, so it
stays on his Mac and is not in git (`.gitignore`). Anywhere else, the
description above stands in for it.*

**What "exactly like the Mac" needs from the window server:**

| Part | What it does | What it needs |
|---|---|---|
| The way in | the hot corner (top right, `docs/ui-theme.md`'s page curl), F3 or Ctrl+↑, a three-finger swipe up (docs/TOUCHPAD-PLAN.md), the Start menu | the window server sees the pointer, the keys, and libinput's gestures |
| The zoom | ~250 ms from the desktop to the overview and back; windows keep their contents and **keep updating** (a video keeps playing) | **every window's pixels kept**, even those covered by others, and drawn scaled 60 times a second |
| The layout | no overlaps, close to where each window was, a title under each, the largest windows largest | the window list, sizes and positions (`docs/DESIGN.md`'s overview has the algorithm) |
| The Spaces bar | one live miniature per desktop; **+** adds one; drag a window onto a desktop to move it there; click a desktop to go to it | **the pixels of windows on other desktops too**, which are not on the screen at all |
| Leaving | click a window: it comes forward and everything zooms back. Esc or the corner: back as it was | focus and stacking |

**How it affects the plan.** It strengthens the move to our own window
server:
- **In X11, with IceWM:** the overview in `docs/DESIGN.md` can do the
  window layout with any compositor level, and the zoom with picom.
  **The Spaces bar can't be live there:** IceWM *unmaps* the windows of
  other workspaces, and an unmapped X window has no pixels. Making it
  live would mean replacing IceWM with a compositing window manager of
  our own, which is a window server written for X.
- **In our own Wayland window server (§2.5):** the compositor keeps every
  surface's buffer, on every Space, all the time. That's how macOS's
  WindowServer works too. Mission Control is then a mode of the
  compositor's scene: each window gets a scale and a position, animated
  by the display's own frame clock (page-flip events, §2.4). The Spaces
  bar draws the other Spaces' windows scaled down.

  So **Mission Control is designed into the compositor from its first
  version**, not added later:
  - a scene graph with a transform per window;
  - an animation clock tied to the page flips;
  - a damage model that knows the whole screen changes while zooming.
  wlroots' scene API provides the first two.

**What it costs, with and without a GPU.** During the zoom every frame is
the whole screen, redrawn from scaled windows: a 1920×1080 screen is
2 million pixels, every 16 ms.
- **With a GPU driver (§2.6):** each window is a texture; scaling and
  blending are free. That's Mission Control as smooth as the Mac's.
- **On the CPU** (pixman, `WLR_RENDERER=pixman`): bilinear scaling with
  SSE2 at roughly 1–2 ns per output pixel on the MSI's CPU, so 2–4 ms
  per frame. **60 frames a second is within reach.** On a Pentium 4 at
  1024×768 (0.8 million pixels, roughly 10–20 ns each) it is 8–16 ms:
  borderline. Three things make it smooth anyway:
  1. **Scale once, at the start.** When Mission Control opens, make each
     window's half- and quarter-size copies (a box filter, one pass).
     The zoom then draws from the nearest size, almost a plain copy.
  2. **Live contents at a lower rate**, only for windows that change: a
     playing video refreshes its miniature 10–15 times a second, not
     60. Miniatures in the Spaces bar refresh about once a second.
  3. **"Reduce motion"**, as on the Mac: no zoom, the overview appears at
     once. It is chosen automatically when the measured frame time is
     over 16 ms, the same rule the compositor level uses today
     (`docs/DESIGN.md`, `picom --benchmark`), and Settings can change it.
- **Memory:** every window's pixels, kept: four bytes per pixel (ten
  800×600 windows: 18 MiB), plus the half- and quarter-size copies (a
  third more) while Mission Control is open.

**Steps**, each measured: zoom frame times, worst and average, on QEMU,
the MSI and the Pentium 4:
1. **In X11 now:** the overview from `docs/DESIGN.md`, for the current
   desktop: the layout, click to focus, F3 and the hot corner. It works
   with IceWM; there is no Spaces bar yet.
2. **In the window server (§2.5):** Mission Control as a compositor mode:
   the zoom, live windows, the layout, Esc and click.
3. **The Spaces bar:** live miniatures, **+**, drag a window to a
   desktop, click to switch. The taskbar shows no workspaces (as decided
   2026-10-09); Spaces live only here, as on the Mac.
4. **The three-finger swipe** (with docs/TOUCHPAD-PLAN.md) and Reduce
   motion.
5. **With GPU drivers (§2.6):** the GLES2 renderer, and the CPU
   shortcuts above become unnecessary.

### 2.8 Order of work

1. **G1: MIT-SHM on** (test, change the config, measure).
2. **G2: the compositor by the numbers.** Then **G3:** X's start and
   GLX, with Part 1's B6.
3. **G4: deferred I/O for the 32-bit shadow.**
4. **§2.4: `/dev/dri/card0`.** First with X's `modesetting` driver, then
   with `modetest`.
5. **§2.5: the Wayland preview session** with labwc and XWayland. Then
   port `myos-panel`, `myos-desktop`, the tray icons and Settings, one by
   one.
6. **§2.6: GPU drivers.** i915 first: the MSI and the old Intel laptops.
   amdgpu after it.
7. **§2.7: Mission Control**, alongside these: the X11 overview now; the
   compositor mode and the Spaces bar with §2.5; smooth everywhere with
   §2.6.

Each step has a before/after number in `docs/performance.md`, and the X11
session stays until the Wayland one is better at everything.
