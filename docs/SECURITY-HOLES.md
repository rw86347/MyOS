# Security holes in MyOS

A catalog of flaws that are **still open**, written so a reader can tell
what an attacker can actually do. This is a hobby kernel. It is not a
security product. Users exist on paper; they are not a boundary.

Network parsers and hostile-disk work that **already got a fix** live in
`docs/security.md` (threat model, rules, `make attacks`). Shortcuts that
are not specifically attacks live in `docs/gaps.md`. This file is the
holes that remain.

Checked against the code on 2026-10-09 (live installer and proc review added
the same day).

## The short version

Anyone who can run a program on the machine is root in all the ways that
matter. They can write `/etc/shadow`, kill any process, mount, reboot, and
watch the keyboard on `:0`. The login screen and `su` accept **any typed
password** for the live accounts, because those accounts are empty and
PAM's `nullok` treats that as "yes". On the **live disk**, `myos` is in
**wheel** and `doas` is **`permit nopass`**, so that login is also instant
**Alpine root** without a second prompt. The setuid **`myos-install-helper`**
does not ask who is calling it: when block devices exist, **`--apply`**
can repartition a disk from any local account. Chromium and Firefox run
without a sandbox. The kernel does not enforce NX, ASLR, or `PROT_NONE`.

The network stack is the one place that has been gone over on purpose.
Off-path guessing (ISN, ports, DNS ids) was closed. Same-LAN ARP races,
raw ICMP sniffing, and a weak boot-time CSPRNG are still open.

## 1. There is no user boundary

Processes have a uid, groups, and setuid exec (`kernel/proc.c`). Almost
nothing consults them.

| What Linux checks | What MyOS does |
|---|---|
| `open` / `unlink` / `chmod` / `chown` | Stored on the inode; `ls -l` shows them. Not checked. The live user can write `/etc/shadow`. |
| `kill` another uid's process | No check. |
| `mount`, `reboot`, `sethostname` | Anyone. |
| `execve` | Needs an execute bit. That is the one file check. |
| Mount flags `nosuid` / `nodev` / `noexec` | Accepted. Not enforced. |
| Capabilities | `capset` that drops any is refused. `ping` that drops to `CAP_NET_RAW` fails. |
| seccomp, namespaces, landlock | None. |

Setuid binaries therefore do not create a privilege boundary on the
**MyOS kernel**: the caller already has every right the binary would gain.
`docs/gaps.md` ("Permissions are stored, not checked"). On the **Alpine
live** side, setuid **does** matter: `myos-install-helper` and
`myos-adduser` run as root when invoked (see §2 and §10).

A few kernel paths **do** look at uid (they are the exception, not the
rule):

| Check | Where |
|---|---|
| `chroot` | Root only (`kernel/syscall.c`). |
| `mknod` for char/block devices | Root only (`kernel/sys_fs.c`). |
| `mount` / `umount` of a disk file system | Root only (`kernel/sys_fs.c`). |
| UDP `bind` to port &lt; 1024 | Root only (`net/socket.c`). |
| SysV IPC `msg*` / `sem*` / `shm*` | `ipc_may()` on the object's mode and owner (`kernel/ipc.c`). |
| Another process's `/proc/.../maps`, `environ`, `io` | Reader must be that uid or root (`fs/procfs.c`). |

Everything else in the table above—including **`open` on `/etc/shadow`**,
**`kill`**, **`reboot`**, and **`chmod`**—still has no gate.

`vhangup` (`drivers/tty.c`) sends SIGHUP but does not revoke open
descriptors. A process that kept the console open can read the next
person's typing.

## 2. Passwords on the live disk

`root` and `myos` are baked in with an empty shadow field
(`tools/fetch-live.py`). Alpine PAM is built with `nullok`, so
**pam_unix accepts any string** for those accounts: the graphical login,
`su`, and anything else that uses `base-auth`. `myos-adduser` refuses a
typed password for an empty account; the login screen does not.

sshd is the exception: `PermitEmptyPasswords no` and `PermitRootLogin no`
(`live/overlay/etc/ssh/sshd_config.d/10-myos.conf`). Empty-password SSH
is refused. A key in `/etc/ssh/authorized_keys/myos` still gets in.

There is still no tool to change a password after boot. The installer is
supposed to set real ones (or lock root). Until it does, sitting at the
machine is enough.

No syslog daemon reads the UNIX log socket, so failed logins go nowhere.

**`doas` on the live disk** (`live/overlay/etc/doas.d/wheel.conf`):
`permit nopass :wheel`. The live user is in `wheel` (`/etc/group`). After
any successful login, **`doas <command>` is root with no password**. The
installer is meant to replace this with `permit persist :wheel` on an
installed system (password then required). Until then, nullok login plus
nopass doas is the whole privilege model on Alpine.

## 3. The X server has no access control

`myos-login` starts X as `:0` with `-nolock` and **no `-auth` file**.
Any local process of any uid can connect to `/tmp/.X11-unix/X0` and:

- read keystrokes (passwords typed into a window included)
- take screenshots
- inject input

SSH into the live user and `DISPLAY=:0 chromium` works for this reason.
`tools/qemu-desktop.py`'s `xdo` relies on it. The Linux fix is a cookie
only root can read, `X -auth`, and `~/.Xauthority` for the session
(`docs/gaps.md`, 2026-10-09).

`-nolisten tcp` is set, so this is not open on the LAN. It is open to
every process on the box.

## 4. Browsers have no jail

Chromium is started with `--no-sandbox`
(`live/overlay/etc/chromium/myos.conf`). The sandbox wants seccomp-bpf
and user namespaces. Without them Chromium refuses to start, so the flag
is required. A bug in the renderer is then the user's rights, not a
helper's.

Firefox, when it is on the disk, sets `MOZ_DISABLE_CONTENT_SANDBOX` and
`MOZ_DISABLE_GMP_SANDBOX` for the same reason.

## 5. Memory: no ASLR, no NX, no guard pages that work

| Defence | Status |
|---|---|
| Address-space randomisation | None. Static PIEs load at `0x00400000`. The dynamic linker sits at the first free mmap hole. |
| `PROT_NONE` | Not enforced. The page stays present and readable. Guard pages and Chromium-style reservations never fault. |
| No-execute (NX / XD) | Not set on either CPU. i386 without PAE cannot. x86_64 can and does not. User pages and kernel pages are executable. |
| Kernel image W^X | The whole kernel mapping is writable and executable. A stray kernel write can change kernel text. |
| Stack canaries | Not built (`-fstack-protector-strong` is on the plan in `docs/security.md`). |
| SMEP / SMAP | Not used. The kernel can run or read user pages. |
| Boot thread stack | "main" has no guard page (it lives in a big-page mapping). Other threads do. |

A buffer overrun in user space that reaches a return address, or a kernel
write that lands in `.text`, is not stopped by the page tables.

`madvise(MADV_DONTNEED)` now drops pages; that was a correctness hole
(stale secrets staying resident), not an access-control one.

## 6. Privilege the kernel never asks for

Anyone may:

- open a **raw ICMP socket** and read every ICMP packet on the machine,
  including another program's ping replies. Sending is limited to echo
  request (so this is not a smurf / redirect kit). Linux wants
  `CAP_NET_RAW`.
- `bind` a TCP or raw socket: the call returns 0 and does nothing. A
  program that thinks it owns a port does not.
- `setsockopt`: most options are accepted and ignored (`SO_RCVTIMEO` and
  `SO_SNDTIMEO` work since 2026-10-14, except on netlink and packet sockets).

Ports below 1024 on `listen` do require root. That is one of the few
uid checks that exist.

## 7. The network, still open

What is **closed** (see `docs/security.md`): predictable ISN / ports /
DNS ids / IP IDs, unsolicited ARP cache poisoning, broadcast ping
reflection, most parser length bugs that were found, per-peer rate
limits. `make attacks` and `make fuzz` cover those parsers.

What is **not**:

- **Same-LAN ARP race.** After we broadcast a request, the first reply
  wins. A neighbour who answers faster than the gateway owns that MAC
  until the 60 s expiry. No stack without authenticated neighbour
  discovery closes this.
- **Sending to broadcast.** Receive of 255.255.255.255 is blocked except
  the DHCP window. `ip_send` still lets a unicast-to-gateway carry a
  broadcast destination.
- **DHCP never renews.** After the lease the machine keeps the address.
  On a network that reassigns it, this host is using someone else's IP.
- **CSPRNG entropy is guessed, not measured.** Neither target CPU has
  RDRAND. The boot key is jitter on ISA port 0x80. Honest claim:
  "probably enough for 128 bits on real hardware". **QEMU with `-icount`
  produces the same key every boot.** Do not treat emulator secrets as
  secrets.
- **One rate-limit table, 16 slots.** Two peers can share a slot. A
  spoofing flood can evict a victim's slot. An ICMP flood from a peer
  also suppresses RSTs to that peer.
- **No IPv6, no fragments, no IP options.** Deliberate. Those are attack
  surface we do not have. When IPv6 arrives, extension headers are the
  dangerous part.
- **No TLS in the kernel**, and no packet filter. Confidentiality is
  user-space TLS or nothing. There is no netfilter.

A local program can flush the four-slot ARP wait queue by talking to many
unresolved on-subnet addresses. TCP retransmits; the cost is delay.

## 8. Disks and USB

`storage_init` mounts an ext2 volume labelled `myos-root` or `myos-data`
**read-write** as soon as it appears. Plugging in a stick is the whole
attack: the parser runs before any user exists to say no.

ext2 has a host fuzzer (`make diskattacks`, `make diskfuzz`) and a list of
fixed length / bitmap / rename bugs in `docs/security.md`. Still open:

- **FAT32 has had no such pass.** `fs/fatvfs.c` reads a boot sector, FAT
  and directories from the same untrusted device.
- **No journal.** Power loss mid-write can leave the volume inconsistent.
- **No permission check on the mount**, and no way to refuse a label.

A crafted image is as real as a crafted packet. Treat unknown USB sticks
as hostile.

## 9. Kernel hardening that is not there

From `docs/security.md` plan and `docs/gaps.md` (2026-10-08):

- No `-fstack-protector-strong` on the kernel.
- No Spectre v1 barriers on user-controlled indexes (syscall number, fd).
  No retpolines. Pentium 4 and Pentium M are affected.
- `MEMDEBUG` catches writes to freed objects, not overruns into a live
  neighbour.
- `struct file` serial numbers are 32-bit. After 2^32 opens, a collision
  plus the same heap address and fd number makes epoll watch the wrong
  file. Not a smash; a mix-up.

Copy-to-user races (pipes, sockets, `getdents64`, wait, tty) were found
and pinned. Those fixes have `make verify-racetests`. The tty path still
has no attack-path test of its own. Socket pinning has no test that
fails when the pin is removed.

**`/proc` modes are not enforced.** Inodes such as `sysrq-trigger` are
created mode `0200` (`fs/procfs.c`), but **`vfs_open_at` does not check
mode or owner** (§1). Any process can open them and write. Today that
means **`thread_dump()`** via sysrq `'t'` (register and wait queues to
serial). **`oom_score_adj`** accepts writes from anyone; the value is not
kept yet, but the permission model is wrong for when OOM policy exists.

**Other users' `/proc/<pid>/` data** is only partly gated. `maps`,
`environ`, and `io` require the reader to be that uid or root. **`stat`,
`status`, `cmdline`, and `comm`** for another live process are still
readable—fine while there is no boundary, wrong once there is.

## 10. Live user-space: installer and plans

These are **Alpine / live** holes. They matter even when the MyOS kernel
has no permission checks, because the stick spends most of its time running
Linux user-space as root through setuid and `doas`.

### Setuid `myos-install-helper`

Built setuid root (`Makefile`, `chmod 4755`). **`--apply`** and
**`--dry-run`** call `setuid(0)` / `setgid(0)` with **no check** of who
invoked the helper—no wheel, no password, no comparison to the user who
wrote the plan (`live/myos-install/myos-install-helper.c`).

What a local user can do:

- Run **`myos-install-helper --apply /path/to/plan`** (or **`myos-install
  --apply FILE`**, which execs the same helper) once **`/dev/sda`** (or
  similar) exists on MyOS.
- The helper re-parses the plan, runs **`plan_check`**, refuses
  world-readable plans that contain a password (mode not `0600`), and
  **refuses the live USB stick** if `disks_scan` marks it `is_stick`.
- It does **not** require a GUI confirmation string. If the chosen disk is
  **missing from sysfs**, it logs a **warning** and **still proceeds** with
  **`sfdisk`**, **`mke2fs`**, mount, **`rsync`**, and config writes when
  `apply` is set.
- **`--dry-run`** also elevates to root even though it mostly skips writes;
  that is unnecessary attack surface.

`--check` does not setuid; it only validates a plan.

### Plan contents the helper trusts

`plan_check` validates **timezone** (`plan_good_zone`), **user**, **hostname**,
**compositor** (`none` / `xrender` / `gl`), and **minimum root size**. It
does **not** validate **`lang`** or **`keyboard`** beyond "non-empty".
Those strings are copied into files on the target root (`step_configure`):

- **`/etc/profile.d/lang.sh`**: `export LANG=…` / `LC_ALL=…` — a newline
  in `lang` becomes a new shell line (command injection when the file is
  sourced).
- **`/etc/X11/xorg.conf.d/00-keyboard.conf`**: `Option "XkbLayout" "…"` —
  unescaped `"` or newlines break or extend the config.

**`wifi_ssid`** is also unvalidated if `copy_wifi` is set (logging only
today; future copy steps could inherit the same bug class).

**Disk path** must start with `/dev/` but is not required to match
**`disks_scan`** except for the stick check. A plan can name **`/dev/sdb`**
while the UI thought **`/dev/sda`**.

### Plan file TOCTOU

**`open_plan`** opens the path with **`O_NOFOLLOW`**, **`fstat`s** it,
closes the fd, then **`plan_read(path)`** opens the path again. An attacker
with a race on a shared directory (e.g. **`/tmp`**) can swap the file
between the stat and the second open—replacing a good plan with a malicious
one, or flipping permissions after a mode check.

### Install log

The helper opens **`/tmp/myos-install.log`** and **`fchmod(..., 0644)`**.
It does not log the password, but it does log disk names, **`sfdisk`**
scripts, hostnames, and account names—readable by every local user on a
multi-user live session (unusual today, easy to forget later).

### Installer does not create accounts yet

**`step_configure`** writes hostname, locale, keyboard, **`doas`**, and
timezone on the mounted target root, then **logs** that it would add the
user to passwd/shadow—it does **not** call **`myos-adduser`** on the new
root yet. A finished install can ship **without the password the UI
collected**, while **`doas.d/wheel.conf`** on the target is already
**`permit persist :wheel`**. That is both a correctness bug and a security
story gap (operators assume the wizard locked root and set the user).

## 11. Desktop tools that trust X

**Force Quit** (`live/myos-forcequit/myos-forcequit.c`) lists windows from
**`_NET_CLIENT_LIST`** and reads **`_NET_WM_PID`** from each. Any X client
on `:0` can claim an arbitrary PID. After the user confirms, the tool sends
**`SIGKILL`** (and **`kill(-pgid, SIGKILL)`** when the pid equals its
process group and is not the tool's own group). Wrong or malicious PIDs
mean killing the wrong program—or any pid the user may signal on Linux.
There is no verification that the pid owns the window.

This sits on top of §3 (no X auth): every local process is already a full
X client.

**Trash / `myos-rm`**: **`--putBack`** restores using a sidecar **`from=`**
path stored when the file was moved to **`~/Trash`**. Names must not
contain **`/`**. Tampering with sidecars in one's own Trash only affects
that user's restores; cross-user abuse needs write access to their home.

## 12. What an attacker can do, by where they sit

**At the keyboard, live disk, no password set.** They are in. Type
anything at `myos` or `root`.

**A program they can run (including a page in unsandboxed Chromium).**
Read and write every file, including shadow and other users' homes. Kill
anyone. Connect to X and steal keystrokes. Open raw ICMP. Reboot.

**SSH as `myos` with a key, desktop already up.** Same as above, plus
they share `:0` with whoever is at the screen.

**On the same Ethernet / Wi-Fi.** Poison ARP during a request. Answer
DHCP (the client checks xid and a few address classes; it still believes
the rest). See traffic; there is no kernel TLS.

**Off-path on the Internet.** Guessing ISN / ports / DNS ids is no longer
the easy path. Malformed packets hit parsers that have been fuzzed; that
is not a proof they are correct. There is still no listen-table SYN-flood
story beyond SYN cookies already in the stack and a 16-connection cap.

**A USB stick they plug in.** The kernel mounts it if the label matches.
ext2 is the well-reviewed case. FAT32 is not.

**Any local user on the live disk, logged in as `myos`.** **`doas sh`**
with no password (wheel + nopass). Run **`myos-install-helper --apply`**
with a crafted plan when block devices work—potential **disk wipe** without
the GUI. Read **`/tmp/myos-install.log`** from someone else's install
attempt.

## 13. What to do first, if the goal is a boundary

Order is the useful one, not the impressive one:

**Live disk (Alpine), before or in parallel with kernel ACLs**

1. **`doas`**: drop **`nopass`** on the live image; require a real password
   or keep **`myos`** out of **`wheel`** until post-install.
2. **Install helper**: do not setuid the whole binary—authenticate (same
   **`admin_ok`** / wheel + password as **`myos-adduser`**); never setuid
   for **`--dry-run`**; hard-fail if **`p->disk`** is not in
   **`disks_scan`**; parse the plan from the **already-open fd** (fix
   TOCTOU); **`plan_good_lang`** / **`plan_good_keyboard`**; install log
   **`0600`**; require explicit confirm before **`sfdisk`**.
3. **Empty passwords:** refuse a typed password for an empty account in
   **`myos-login`** the way **`myos-adduser`** already does. Lock **`root`**
   on the live disk (`root:*:`). **`step_configure`** must actually create
   the user on the target root with the chosen password.
4. **Xauthority** on `:0`.

**MyOS kernel**

5. **Check permissions** on `open`, `unlink`, `kill`, `mount`, `reboot`,
   `sethostname`. Until that lands, nothing else is a user boundary on the
   kernel.
6. **Enforce inode mode/owner** on special nodes (`/proc/sysrq-trigger`,
   etc.) when (5) exists—or ad-hoc checks in procfs until then.
7. **NX on x86_64** (CPUID XD) and W^X on the kernel map. **`PROT_NONE`**
   that actually faults. ASLR can wait until there is a boundary worth
   randomising.
8. **Root-only `SOCK_RAW`**, once `ping` has another way to send echoes
   (or a tiny helper).
9. **Chromium sandbox** when seccomp and user namespaces exist. Until
   then, treat the browser as the user's full account.
10. **FAT32 disk attacks**, same harness as ext2.
11. Stack canaries and SMEP/SMAP, measured (`docs/security.md` plan).

Do not add a capability system before kernel (5). `capset` that cannot
drop rights is already lying.

## See also

- `docs/security.md` — network threat model, parser rules, fixed bugs,
  fuzzing, the CSPRNG.
- `docs/gaps.md` — every known shortcut, including the ones that are not
  attacks.
- `docs/performance.md` — cost of the copies and of hardening, when it
  is measured.
