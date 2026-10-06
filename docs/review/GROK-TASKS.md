# Work for Grok — MyOS, after the x86_64 port

Written by Claude on 2026-10-05, for Grok. Rodger owns the project; Claude reviews your
work. Read this whole file before you start, then do the tasks one at a time in this
order: **T00, T01, T02 – T07, T09 – T11**. (T08 and T12 are Claude's now: the x86_64
live disk needed them first. Their text stays below for reference; skip them.)

Other AIs work in this same folder too (Cursor rebuilds the live disk images, Claude the
kernel). That is why you add files to git by name, and never touch files outside your task.

MyOS is a small kernel that runs unmodified Linux programs on two CPUs: **i386** (32-bit,
the old Pentium 4 PCs it is tuned for) and **x86_64** (64-bit, a newer laptop). One source
tree; CPU-specific code lives only in `arch/i386/` and `arch/x86_64/`. Programs talk to the
kernel through **Linux's system call interface**: the same numbers, the same argument
order, the same struct layouts as Linux, on each CPU.

---

## 1. Before every session

1. `git status` and `git log --oneline -5` — know where you are.
2. Read `docs/review/grok-notes.md` from the top: **Claude answers your questions there**
   (lines starting `Claude:`). Do what they say before starting anything new.
3. Read `CLAUDE.md` (the project rules) if you haven't this session.

## 2. Rules (these are not suggestions)

**Git**
- Work on the branch `grok/work`. Create it once: `git checkout master && git checkout -b grok/work`.
  Never commit to `master`, never merge, never rebase, never force-push, never delete branches.
- **One commit per task** (T03 has sub-tasks: one commit each). Message: `grok T06: msync and mlock*`.
- Add your files by name: `git add kernel/syscall.c user/sys/systest.c ...`.
  **Never `git add -A` or `git add .`** — there may be other people's files in the tree.
- If you can't finish a task: commit what you have with `(INCOMPLETE)` in the message,
  say why in the notes, and move on to the next task.

**Files you may change**
- `kernel/syscall.c`, `kernel/sys_fs.c`, `kernel/sys_info.c`, `kernel/ipc.c`, `kernel/sem.c`,
  `kernel/msg.c`, `kernel/shm.c`, `fs/epoll.c`, `net/socket.c` — only for the task at hand.
- `include/syscall.h` (i386 numbers), `include/syscalls.h` (handler declarations).
- `user/sys/systest.c` and new files in `user/sys/`, new files in `tools/`.
- `Makefile`: only test targets (`test-uefi-shell` and new targets the tasks ask for).
- `kernel/*_test.c`: only in T04, only to make tests compile/run on x86_64.
- T01 only: the arguments of `kprintf`/`klog` calls anywhere.

**Files you must NOT change** (Claude's; ask in the notes instead)
- Anything in `arch/`, `mm/`; `kernel/signal.c`, `kernel/proc.c`, `kernel/thread.c`,
  `kernel/sched.c`, `kernel/elf*.c`, `kernel/usermode.c`, `kernel/kprintf.c`; `fs/vfs.c`,
  `fs/inotify.c`, `fs/lock.c`; `drivers/evdev.c`; `include/user.h`, `include/vfs.h`,
  `include/signal.h`, anything `uaccess`; `docs/` except your notes file.
- If a task seems to need one of these: **stop**, write what you'd change and why in the
  notes (with the exact lines), and go on to the next task.

**Correctness**
- Syscall numbers must match Linux exactly. x86_64: `include/syscall64.h` already lists them
  all. i386: `include/syscall.h`, and Linux's `arch/x86/entry/syscalls/syscall_32.tbl`
  (you may read it online; you may not download things into the build).
- Struct layouts must match Linux exactly, **on both CPUs**. Every struct you write for user
  memory gets a `_Static_assert(sizeof(...) == N, "...")` per CPU, and a check in
  `user/sys/systest.c` that would fail if a field were in the wrong place.
- Behaviour must match the Linux man pages (man7.org, section 2): return values, errno
  values, edge cases. Put the man page's name in a comment above each new syscall.
- Never weaken or delete an existing test or check to make something pass.
- **No downloads**: no `curl`, no `brew`, no `tools/fetch-*`. If something needs one, ask
  Rodger in the notes.
- No new compiler warnings on either CPU (until T01 is done, don't add to the format ones).

**Style**
- This is a teaching project: comments explain *why*, in plain language, like the code
  around them. Read the neighbouring functions first and write like them.
- Speed matters: don't add work to read/write/open/close, the syscall entry or the timer
  tick. If you think you must, ask first.

## 3. How to report your work (for Claude's review)

**The notes file:** `docs/review/grok-notes.md`. After every task, append an entry using the
template at the top of that file: what you changed, why, how you tested it (paste the last
lines of each test's output), what you're unsure about, and questions. Be honest — "I don't
know why this works" is useful; a wrong guess presented as fact is harmful.

**In the code:** where you made a choice you're not sure of, leave a comment

    /* REVIEW(grok): <your question or doubt, one or two lines> */

Claude finds these with `grep -rn "REVIEW(grok)"` and removes them after review. Don't use
the word REVIEW for anything else.

**Known kernel bugs your tests find** (not yours to fix): write them in the notes under
"Kernel bugs found" with the failing check, what Linux does, what MyOS does. In
`systest.c` use `xfail(...)` for that check (T03 tells you how), so the test target stays
green and the bug stays visible.

## 4. How to build and test

    make                          # i386 kernel + images
    make ARCH=x86_64              # x86_64 kernel + UEFI image
    make test                     # i386: self-tests in QEMU            (~4 min)
    make test-shell               # i386: shell commands, disk           (~3 min)
    make test-hd                  # i386: an installed disk              (~3 min)
    make test-uefi-shell          # i386 under UEFI: systest + shell     (~2 min)
    make ARCH=x86_64 test-uefi-shell   # the same on x86_64             (~2 min)

- **Quick list** (before every commit): both builds with no new warnings, and both
  `test-uefi-shell` runs.
- **Full list** (before every commit that changes kernel C code, i.e. not only
  `user/sys/` or `tools/`): the quick list plus `make test`, `make test-shell`, `make test-hd`.
- A test prints `... TEST OK` at the end when it passes. Logs: `build/uefi-shell.txt`,
  `build/x86_64/uefi-shell.txt`, `build/test/serial.log`, `build/serial.txt`.
- To see **all** warnings (make only shows them for files it recompiles):

      find build/x86_64 -name "*.o" -not -path "*/initramfs*" -not -path "*/user/*" | xargs rm -f
      make ARCH=x86_64 2>&1 | grep "warning:"

  (Use `build` instead of `build/x86_64`, and plain `make`, for i386.)
- Try things by hand: `make ARCH=x86_64 run-uefi` opens a QEMU window with a shell; type
  in the window (the serial port is output only). `make run-uefi` for i386.
- A kernel crash prints an instruction address. Find the source line:
  `x86_64-elf-addr2line -e build/x86_64/kernel.elf <RIP>` or
  `i686-elf-addr2line -e build/kernel.elf <EIP>`.

## 5. What you need to know about the code

**System calls** live in `kernel/syscall.c`:
- Handlers written in that file use `SYSCALL(sys_name) { ... }`; handlers in other files use
  `SYSCALL_DEF(sys_name)` and are declared in `include/syscalls.h`. Every handler gets six
  arguments `a1`..`a6` of type `uintptr_t` (32 bits on i386, 64 on x86_64) and returns
  `intptr_t`: the result, or `-ERRNO` (e.g. `return -EINVAL;`).
- Two tables at the end of the file: `syscall_table[]` (i386, indexed by `NR_*` from
  `include/syscall.h`) and `syscall_table64[]` (x86_64, indexed by `NR64_*` from
  `include/syscall64.h`). **A new syscall goes into both tables** (when it exists on both
  CPUs; the numbers differ).
- Where the two CPUs' ABIs differ, an x86_64 adapter sits in the `#ifdef ARCH_X86_64`
  block before the tables (look at `sys_mmap_x64`, `sys_pread_x64`, `sys_clone_x64`).
  The classic difference: i386 passes a 64-bit file offset as two 32-bit arguments
  (low, high); x86_64 passes it in one.

**User memory** — a pointer from a program is a number, never dereferenced directly:
- `copy_from_user(kernel_dst, (const void *)a2, n)` / `copy_to_user((void *)a2, kernel_src, n)`
  return 0 or `-EFAULT`.
- `get_path(buf, a1)` copies a path in. `get_user_word(&w, addr)` reads one pointer-sized word.
- `fd_get(a1)` gives the `struct file *` for an fd, or NULL (then `return -EBADF;`).
- Always keep user pointers in `uintptr_t` (or `user_addr_t`). **Never** store one in a
  `uint32_t` or `int`: on x86_64 that silently cuts it in half. This exact bug was found in
  dozens of places last week.

**Struct layouts that work on both CPUs:** write Linux's struct with `long` / `unsigned long`
where Linux has `long` (or `__kernel_long_t`), `uint32_t`/`int32_t` where Linux has `int`,
and `user_addr_t` for pointers and `size_t`. The compiler then pads it the way Linux does on
each CPU. Examples to copy: `struct timex_long` in `kernel/sys_info.c`, `struct statfs_long`
in `kernel/sys_fs.c`, `struct user_iovec` and `struct user_msghdr` in `include/user.h`.
When Linux's two layouts differ in other ways (i386 often has extra `_high` words for times),
write two structs inside `#ifdef ARCH_X86_64 / #else / #endif`, each with its
`_Static_assert` — copy `struct msqid_ds_user` in `kernel/msg.c`.

**The test program** `user/sys/systest.c`: a program with no C library that makes raw
system calls on either CPU and checks the results. It has an `enum` of syscall numbers per
CPU at the top, `SC0`..`SC6` macros to call them, and `check(ok, "name", detail)`. Each
area is a function `t_something()` called from `cmain()`. `make test-uefi-shell` (and the
x86_64 one) boots a test image, runs `systest`, and fails unless it prints `systest: all ok`.
Its output is in the `uefi-shell.txt` logs above.

**musl's choices on i386** — what a real program calls, so what to test: stat → `statx`
(383); lseek → `_llseek` (140); mmap → `mmap2` (192, offset in pages); fcntl → `fcntl64`
(221); clock_gettime → `clock_gettime64` (403); nanosleep → `clock_nanosleep_time64` (407);
futex → `futex_time64` (422); ppoll → `ppoll_time64` (414); timespecs are two 64-bit
numbers there. On x86_64 the plain calls, and a timespec is two `long`s.

---

## 6. The tasks

Difficulty: ★ easy, ★★ medium, ★★★ harder (go slowly, leave REVIEW comments).

### T00 ★ — Get set up

1. Create the branch (section 2). Build both CPUs. Run the **full list** of tests (section 4)
   on the unchanged tree.
2. Write the first notes entry: "T00 baseline", with the result line of each test. If any
   fail on the unchanged tree, stop and say so in the notes.

No commit for T00 except the notes file: `grok T00: baseline`.

### T01 ★ — Fix the format warnings on x86_64

`make ARCH=x86_64` gives 45 `-Wformat` warnings in 16 files (e1000.c, pmm.c, ext2.c,
pcache.c, ata.c, partition.c, ahci.c, bcache.c, nvme.c, arch/x86_64/cpu.c, syscall.c,
main.c, initrd.c, virtio_blk.c, pci.c, drm_fbdev.c). Use the "see all warnings" commands in
section 4 to list them.

Why they happen: the code prints `uint32_t` values with `%lu`/`%lx`. With the i386
compiler `uint32_t` *is* `unsigned long`, so that's right there; with the x86_64 compiler
it's `unsigned int`, and `%lu` reads 64 bits.

The fix, the same everywhere: **cast the argument**, don't change the format.

    kprintf("disk: %lu sectors\n", count);                    /* before */
    kprintf("disk: %lu sectors\n", (unsigned long)count);     /* after  */

For `%ld` use `(long)`. For a `uint64_t` printed with `%llx`/`%llu` use `(unsigned long long)`.
Do not touch `kernel/kprintf.c`, and change only arguments, nothing else on those lines.
(`arch/x86_64/cpu.c` is the one file in `arch/` you may edit, for this task only.)

Done when: zero `Wformat` warnings on **both** CPUs after a full clean rebuild (section 4),
quick list passes. One commit.

### T02 ★★ — `tools/syscall-scan.py`: which syscalls can a program make?

Claude found the missing syscalls last week by hand: disassemble a binary, find every
system call instruction, read the number loaded into EAX just before it, compare with the
kernel's tables. Make that a tool, so anyone can run it on any program before trying it.

    python3 tools/syscall-scan.py build/x86_64/initramfs/bin/busybox
    python3 tools/syscall-scan.py --arch i386 path/to/libc.musl-x86.so.1 path/to/Xorg

What it does:
1. Detect the CPU from the ELF header (byte 4: 1 = 32-bit, 2 = 64-bit), or `--arch`.
2. Disassemble with `i686-elf-objdump -d` or `x86_64-elf-objdump -d`.
3. Find system call instructions: `syscall` (x86_64); `int $0x80` and `call *%gs:0x10`
   (i386; the second is musl's fast path).
4. For each, look backwards (at most ~40 lines, and never past the start of the function,
   a line like `00401eb9 <name>:`) for `mov $0xNN,%eax`. If another instruction writes
   `%eax` first (`xor %eax,%eax` means 0), use that; if you can't tell, count it as
   "unresolved". Ignore numbers above 500 (they're not syscall numbers).
5. Read what the kernel implements: the `#define NR_xxx N` lines in `include/syscall.h`
   and `#define NR64_xxx N` in `include/syscall64.h`, and the `[NR_xxx] = ...` /
   `[NR64_xxx] = ...` entries inside `syscall_table[]` and `syscall_table64[]` in
   `kernel/syscall.c`.
6. Print: the numbers used, split into **implemented** and **missing**, with names (x86_64:
   from syscall64.h; i386: from syscall.h, or `?` if it isn't there), and the count of
   unresolved sites. `--missing-only` prints just the missing ones.

Check it against what Claude found on today's tree (c9a8b03): x86_64 busybox missing
exactly 235 (`utimes`) and 261 (`futimesat`); i386 busybox missing 195 (`stat64`), 197
(`fstat64`), 264 (`clock_settime`, 32-bit), 271 (`utimes`), 299 (`futimesat`), 300
(`fstatat64`) — all old fallbacks musl only tries after the modern call fails, which the
kernel leaves out on purpose (docs/DESIGN.md). Claude's quick script also reported a `0`
on i386: probably an `xor %eax,%eax` that isn't a syscall number; see whether yours does
better. Put the full output for both busyboxes in your notes.

Pure Python 3, standard library only, comments at the top saying what it does and its
limits (it can't see numbers computed at run time). One commit.

### T03 ★★ — More checks in `systest.c` for syscalls that already exist

Most of the syscalls the kernel has were only ever tested on i386, often through a C
library that hides layout mistakes. Write checks that call them directly on **both** CPUs.
This is the most valuable task here: it finds bugs before real programs do.

First, add an expected-failure helper next to `check()`:

    /* A check that fails because of a kernel bug already reported in
       docs/review/grok-notes.md: shown, not counted. */
    static void xfail(int ok, const char *name, long detail)
    {
        out(ok ? "XPASS " : "XFAIL "); out(name); out(" ("); outn(detail); out(")\n");
    }

Use `xfail` only for a failure you have confirmed is the kernel's fault and written up
under "Kernel bugs found". If you're not sure whether your test or the kernel is wrong,
say so in the notes and use `xfail` anyway — Claude decides.

Do these as five commits, `grok T03a` .. `grok T03e`. For each syscall add its number to
**both** enums at the top of systest.c (x86_64 from `include/syscall64.h`; i386: the one
musl uses, see section 5). Work in `/tmp/systest/...` for files you create, and clean up.

- **T03a files**: `openat`, `getdents64` (check `struct linux_dirent64`: `u64 d_ino; s64 d_off;
  u16 d_reclen; u8 d_type; char d_name[]`, same on both CPUs — list a directory you made and
  find your file names), `statx` (256-byte `struct statx`: check `stx_size` at offset 40 and
  `stx_mode` at 28), x86_64 `newfstatat` (144-byte `struct stat`: `st_size` at offset 48,
  `st_mode` at 24), `readlinkat` on a symlink you made, `renameat2` with `RENAME_NOREPLACE`
  (1) onto an existing name (must be `-EEXIST`), `utimensat` (then check the time with
  statx), `fchmod`, `umask`, `ftruncate` (i386: `ftruncate64`, length in two halves),
  `lseek` with `SEEK_END` (i386: `_llseek` writes the result through a pointer),
  `pread64`/`pwrite64` (i386: offset in two halves, a4 = low, a5 = high), `readv`/`writev`
  with 3 buffers (check the `struct iovec` layout really is two pointer-sized words on each
  CPU), `fcntl` `F_GETFL`/`F_SETFL` with `O_NONBLOCK`, `F_DUPFD_CLOEXEC` then `F_GETFD`,
  `memfd_create` + `F_ADD_SEALS` (`F_SEAL_WRITE`) then a write must fail with `-EPERM`.
- **T03b time**: `clock_gettime` for CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1) (monotonic
  never goes backwards across 1000 calls), `clock_nanosleep` 20 ms (measure it),
  `nanosleep` (x86_64 only, 35), `clock_getres`, `gettimeofday` vs `time` (already there:
  extend with the timezone argument).
- **T03c memory**: `mmap` anonymous 3 pages, write, `mprotect` read-only then (in a forked
  child) write → the child must die with signal 11 (check `wait4`'s status: `(st & 0x7f) == 11`);
  `mremap` grow; `munmap`; `brk` grow/shrink; `mmap` of a file you wrote (contents match);
  `mmap` with an offset (x86_64 bytes; i386 `mmap2` pages).
- **T03d processes and signals**: `getppid` in a child, `gettid`, `kill(pid, 0)`,
  `rt_sigprocmask` block SIGUSR1 + `kill` self + `rt_sigpending` shows it + unblock runs the
  handler, `prlimit64` read `RLIMIT_NOFILE` (7), `uname` (sysname "Linux"; machine "x86_64"
  or "i686"), `sched_getaffinity` (bit 0 set), `getrandom` 32 bytes (not all zero),
  `wait4` with `WNOHANG` (1) on a child that is still running (returns 0).
- **T03e pipes, polling, sockets**: `pipe2` with `O_CLOEXEC` (02000000) → `F_GETFD` says
  `FD_CLOEXEC`; `dup3`; `poll` on a pipe (empty: times out; after a write: `POLLIN`);
  `ppoll` with a timeout; `eventfd2` (write 3, read 8 bytes = 3); `epoll_create1` +
  `epoll_ctl` + `epoll_wait` on a pipe (check `struct epoll_event` is **12 bytes, packed** on
  both CPUs: `u32 events; u64 data` — put a recognisable 64-bit value in `data` and check it
  comes back whole); `socketpair(AF_UNIX=1, SOCK_STREAM=1)` + `sendmsg`/`recvmsg` with two
  iovecs (this checks `struct msghdr` — after `recvmsg`, `msg_flags` must be 0 and
  `msg_controllen` must have been set to 0).

Each sub-task: quick list passes (systest must say `all ok`, with your XFAILs). In the notes,
list every XFAIL with the details.

### T04 ★★ — Run the kernel self-tests on x86_64

`make test` on i386 boots a kernel built with `SELFTEST` (the `kernel/*_test.c` files) and
checks the serial log (see the `check-selftests` target in the Makefile). On x86_64,
`make ARCH=x86_64 test` only runs `test-uefi`: the self-test kernel **builds** (`make
ARCH=x86_64 MODE=test`) but has never been run.

1. Add a Makefile target `test-uefi-selftests` (x86_64 and i386) that builds the
   `MODE=test` UEFI image (look at how `MODE=test` changes `BUILD`; the image is
   `$(BUILD)/myos-uefi.img` inside that build directory), boots it under OVMF like
   `test-uefi`, waits long enough (start with 60 s), and greps the log for the self-test
   summary line `Self-tests: N passed, 0 failed` and the other checks from
   `check-selftests` that make sense without a BIOS disk (timer, memory, initramfs, context
   switch, preemption, slot scheduler). Keep it separate from `test`; Claude will wire it in.
2. Run it on x86_64. Expect failures and maybe crashes — **that's the point**.
3. Fix only what is clearly the *test's* problem: an i386 assumption inside a `*_test.c`
   file (a hard-coded 32-bit address, `uint32_t` holding a pointer, `KERNEL_VBASE + x`
   meaning "the direct map" — on x86_64 that's `DIRECTMAP_BASE`). Leave a `REVIEW(grok)`
   comment on each such change.
4. Everything else (a kernel bug, a crash): don't fix. In the notes give the failing test's
   name, the log lines, and for a crash the address and `addr2line` output.

One commit (Makefile + test fixes); a long notes entry.

### T05 ★★ — A shell-test driver that waits for the prompt

The shell tests type commands with fixed `sleep`s between them. When a command is slow, the
next one is typed into it and the output gets mixed — that's why `systest` got a special
8-second wait. Write `tools/qemu-shell.py` that waits for the shell prompt instead.

- Start QEMU with the given arguments plus `-monitor stdio` (or a unix socket) and
  `-serial file:<log>`.
- Wait until the log contains `Welcome to MyOS` and then a prompt (`myos:` ... `# `).
- For each command: send its keys through the monitor (`sendkey ...`, the same mapping as
  `tools/qemu-type.sh`; port it to a Python table), then wait until a *new* prompt appears
  in the log, with a per-command timeout (default 30 s; report a timeout clearly).
- Special command `^C`: send `sendkey ctrl-c`.
- At the end send `quit`. Exit status 0, or 1 on a timeout.
- Write the cleaned log (escape codes and `\r` removed, like the Makefile's `perl` line).

Then make `test-uefi-shell` use it **for both CPUs**, keeping exactly the same `grep`
checks. Run both 5 times in a row; all must pass; put the timings before/after in the
notes. Don't change `test-shell` or the others (they boot differently). One commit.

### T06 ★ — Simple syscalls that have no implementation yet (both CPUs)

Each of these is a few lines: check the arguments the way the man page says, and give the
answer that is true for MyOS (one CPU, no swap, no real-time scheduler classes). Add each
to **both** tables and a check to `systest.c`. Numbers (verify them yourself!):

| syscall | i386 | x86_64 | what to do |
|---|---|---|---|
| `msync(addr, len, flags)` | 144 | 26 | `-EINVAL` if `addr` isn't page aligned, or `flags` has bits other than MS_ASYNC 1 / MS_INVALIDATE 2 / MS_SYNC 4, or both ASYNC and SYNC; else 0 (MAP_SHARED file maps are private copies here, docs/gaps.md: nothing to write back) |
| `mlock(addr, len)` / `munlock` | 150 / 151 | 149 / 150 | 0 (no swap: every page stays in RAM anyway); `-EINVAL` only if `addr + len` overflows |
| `mlock2(addr, len, flags)` | 376 | 325 | like mlock; flags may be 0 or MLOCK_ONFAULT (1), else `-EINVAL` |
| `mlockall(flags)` / `munlockall()` | 152 / 153 | 151 / 152 | flags: MCL_CURRENT 1, MCL_FUTURE 2, MCL_ONFAULT 4; `-EINVAL` if neither CURRENT nor FUTURE, or other bits; else 0 |
| `getcpu(cpu *, node *, cache)` | 318 | 309 | write `unsigned int` 0 to each non-NULL pointer |
| `sched_getscheduler(pid)` | 157 | 145 | `-ESRCH` if no such process (pid 0 = caller), else 0 (SCHED_OTHER) |
| `sched_setscheduler(pid, policy, param *)` | 156 | 144 | policy 0 (OTHER), 3 (BATCH), 5 (IDLE) with `param->sched_priority == 0` → 0; 1 (FIFO), 2 (RR), 6 (DEADLINE) → `-EINVAL` with a comment: MyOS's slot scheduler has no Linux real-time classes; anything else `-EINVAL` |
| `sched_getparam(pid, param *)` / `sched_setparam` | 155 / 154 | 143 / 142 | get: write `int` 0; set: priority must be 0, else `-EINVAL` |
| `sched_get_priority_max(policy)` / `_min` | 159 / 160 | 146 / 147 | FIFO/RR: 99 / 1; OTHER/BATCH/IDLE: 0 / 0; else `-EINVAL` |
| `sched_rr_get_interval(pid, ts *)` | 423 (`_time64`) | 148 | write the length of one scheduler time slot (find it in `kernel/sched.c`; ask in the notes if unclear). x86_64: two `long`s; i386 time64: two `int64_t` |
| `sched_setaffinity(pid, len, mask *)` | 241 | 203 | one CPU: the mask must have bit 0 set, else `-EINVAL`; else 0 |
| `close_range(first, last, flags)` | 436 | 436 | `-EINVAL` if `first > last` or flags other than CLOSE_RANGE_CLOEXEC (4) / CLOSE_RANGE_UNSHARE (2); for each open fd in `[first, min(last, FD_MAX - 1)]`: with CLOEXEC set its close-on-exec bit (`proc_current()->cloexec |= 1ull << fd`), else `fd_close(fd)` |
| `epoll_pwait2(epfd, events, max, ts *, sigmask, size)` | 441 | 441 | like `epoll_pwait` in `fs/epoll.c`, but the timeout is a timespec (NULL = wait forever) instead of milliseconds; round up to whole ms |

Put the handlers in `kernel/sys_info.c` (sched, getcpu), `kernel/syscall.c` (memory ones,
close_range), `fs/epoll.c` (epoll_pwait2). Two commits: `T06a` (memory + getcpu + sched)
and `T06b` (close_range + epoll_pwait2). Full list before each.

### T07 ★★ — `preadv` / `pwritev` / `preadv2` / `pwritev2` (both CPUs)

Read or write several buffers at a file offset, without moving the file position.
`readv`/`writev` (`do_iov` in kernel/syscall.c) do the buffers; `rw_at` does the offset.

- Numbers: i386 333 / 334 / 378 / 379; x86_64 295 / 296 / 327 / 328.
- Arguments: `(fd, iov, iovcnt, pos_l, pos_h)` and for the `2` versions also `flags` (a6).
  The offset: **i386** `((uint64_t)pos_h << 32) | pos_l`; **x86_64** just `pos_l` (Linux
  shifts `pos_h` out entirely on 64-bit — read `pos_from_hilo` in Linux's fs/read_write.c).
- `preadv2`/`pwritev2`: an offset of -1 means "use and move the file position" (like
  readv/writev); flags must be 0, else `-EOPNOTSUPP`.
- Negative offset (other than -1 for the `2` versions): `-EINVAL`.
- Implement once, as a helper next to `do_iov` taking the offset; don't duplicate the loop.
- systest: write a file, `preadv` two buffers from offset 3, check both, check `lseek(fd, 0,
  SEEK_CUR)` didn't move.

One commit, full list.

### T08 ★★ — `copy_file_range` (both CPUs) — TAKEN BY CLAUDE, skip

The x86_64 live disk's first boot (`docs/x86_64-LIVE-HANDOFF.md`) stops because coreutils'
`install` calls it: `syscall 326 not implemented (install)`.

`copy_file_range(fd_in, off_in *, fd_out, off_out *, len, flags)` — copy between two
regular files inside the kernel. `sendfile` in kernel/syscall.c does almost the same: read
it carefully first, especially the comments about `file_copy_out`, `kbuf`, and the private
copy of the destination's `struct file`.

- Numbers: i386 377, x86_64 326. Offsets are `int64_t *` (may be NULL: then use and move
  the file's position).
- `flags != 0` → `-EINVAL`. Both must be regular files (`-EINVAL` otherwise);
  `fd_out` opened with `O_APPEND` → `-EBADF`; same file with overlapping ranges → `-EINVAL`.
- At most `RW_MAX` bytes per call (the same rule as sendfile: a syscall must finish quickly).
- If the destination's `ops->kbuf_ok` is 0 → `-EXDEV` (programs then fall back to read/write).
- Reuse sendfile's loop instead of copying it: factor the loop into a helper both call, if
  you can do it without changing sendfile's behaviour (sendfile's systest checks must still
  pass). If that gets complicated, write a REVIEW comment and duplicate instead.

One commit, full list.

### T09 ★★ — Make `ipcs` work: IPC_INFO and the *_INFO / *_STAT commands

BusyBox `ipcs` lists System V objects with these `semctl`/`msgctl`/`shmctl` commands, which
MyOS answers with `-EINVAL` today (`kernel/sem.c`, `msg.c`, `shm.c`, shared code in
`kernel/ipc.c`, `include/ipc.h`). Read `man 2 semctl`, `msgctl`, `shmctl` first.

| command | value | returns |
|---|---|---|
| `IPC_INFO` | 3 | fills the info struct with the limits; returns the highest slot index in use (0 if none) |
| `SEM_INFO` / `MSG_INFO` / `SHM_INFO` | 19 / 12 / 14 | like IPC_INFO, but with usage counts (see the man pages) |
| `SEM_STAT` / `MSG_STAT` / `SHM_STAT` | 18 / 11 / 13 | the first argument is a **slot index**, not an id; fills the `*_ds` struct like IPC_STAT and returns the object's full id |
| `SEM_STAT_ANY` / `MSG_STAT_ANY` / `SHM_STAT_ANY` | 20 / 13 / 15 | like *_STAT without the read-permission check |

(Yes, MSG_STAT_ANY and SHM_STAT have the same number 13 — they're for different calls.)

The info structs (from Linux `include/uapi/linux/sem.h`, `msg.h`, `shm.h`):
- `struct seminfo` — ten `int`s: semmap, semmni, semmns, semmnu, semmsl, semopm, semume,
  semusz, semvmx, semaem. Same on both CPUs.
- `struct msginfo` — seven `int`s (msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz,
  msgtql) then `unsigned short msgseg`. Same on both CPUs.
- `struct shminfo64` — nine `unsigned long`s: shmmax, shmmin, shmmni, shmseg, shmall, then
  four unused.
- `struct shm_info` — `int used_ids;` then five `unsigned long`s: shm_tot, shm_rss,
  shm_swp, swap_attempts, swap_successes.

Use the limits the code already has (`SEM_SETS`, `SEM_MAX`, `MSG_QUEUES`, `MSGMAX`,
`SHM_SEGS`, ...). The i386 `ipc()` path and the direct calls both lead to `sem_ctl`,
`msg_ctl`, `shm_ctl`; note the `IPC_64` flag handling at the top of each. Then run `ipcs`
in the shell after making a semaphore set and a queue (add the commands to the Makefile's
`UEFI_SHELL_CMDS` with a grep check), and add systest checks.

Two commits (`T09a` sem + msg, `T09b` shm + shell test), full list.

### T10 ★★★ — `sendmmsg` / `recvmmsg` (both CPUs)

Several messages in one call; used by DNS resolvers and network daemons.
`struct mmsghdr { struct msghdr msg_hdr; unsigned int msg_len; }` — with
`struct user_msghdr` from include/user.h that is 32 bytes on i386 and 64 on x86_64 (check
with `_Static_assert`; mind the padding).

- Numbers: `sendmmsg` i386 345, x86_64 307; `recvmmsg` i386 **417** (`recvmmsg_time64`,
  timespec of two `int64_t`), x86_64 299 (timespec of two `long`s).
- Loop over `vlen` entries (cap it at 1024, like Linux's UIO_MAXIOV), calling the same code
  `sys_sendmsg`/`sys_recvmsg` use in net/socket.c (AF_UNIX sockets go to `net/unix.c` —
  look at how `TO_UNIX` routes them; both kinds must work). After each message, write its
  byte count into that entry's `msg_len` with `copy_to_user`.
- Return the number of messages done; stop at the first error, and return the error only if
  nothing was done.
- `recvmmsg` flags: `MSG_WAITFORONE` (0x10000): after the first message, don't wait for
  more. The timeout: give up waiting when it expires (a simple check between messages is
  enough; say in the notes what you did).
- systest: a `socketpair`, `sendmmsg` three messages, `recvmmsg` them back, check the
  `msg_len`s and contents.

Leave REVIEW comments on anything you're unsure of. One commit, full list.

### T11 ★★ — `make ARCH=x86_64 bench`

The benchmark program `user/bench.c` is i386-only (it uses the i386 mini C library). The
project rule is "every speed change comes with a before/after number", and x86_64 has no
numbers. Make a freestanding version in the style of `user/sys/systest.c`
(`user/sys/bench.c`): the same measurements as `user/bench.c` where they make sense
(null syscall, open+close, lseek+read 4 KiB, stat, fork+exit+wait, fork+exec+exit+wait),
timed with `rdtsc`, printed in the same format. Add it to the test image and a
`bench-uefi` Makefile target (both CPUs) that boots it, runs it, and prints the results.
For repeatable numbers QEMU needs `-icount shift=0,sleep=off` (look at `run-bench`).
One commit; put the numbers for both CPUs in the notes.

### T12 ★ — Extended attributes: "not supported", properly (both CPUs) — TAKEN BY CLAUDE, skip

The live disk's first boot also logs `syscall 190 not implemented (install)`. On x86_64,
190 is **`fsetxattr`** (careful: on i386, 190 is `vfork` — the two tables differ, which is
exactly the kind of mistake to watch for). coreutils `install` and `cp -a` copy a file's
extended attributes (SELinux labels, ACLs). MyOS's file systems have none, and the right
answer is the one Linux gives for a file system without xattr support — then those
programs quietly skip them. Today they get `-ENOSYS` ("no such system call") instead.

| syscall | i386 | x86_64 | answer |
|---|---|---|---|
| `setxattr` / `lsetxattr` / `fsetxattr` | 226 / 227 / 228 | 188 / 189 / 190 | `-EOPNOTSUPP` |
| `getxattr` / `lgetxattr` / `fgetxattr` | 229 / 230 / 231 | 191 / 192 / 193 | `-EOPNOTSUPP` |
| `listxattr` / `llistxattr` / `flistxattr` | 232 / 233 / 234 | 194 / 195 / 196 | `0` (an empty list) |
| `removexattr` / `lremovexattr` / `fremovexattr` | 235 / 236 / 237 | 197 / 198 / 199 | `-EOPNOTSUPP` |

But check the arguments first, in Linux's order, so errors are right: the `f*` ones need a
valid fd (`-EBADF`); the path ones need the file to exist (`-ENOENT`, via `get_path` and a
lookup — `l*` without following a final symlink; look at how `sys_lchown32` or `target_of`
in `kernel/sys_fs.c` do it). Then the answer above. One shared helper, not twelve copies.
Put them in `kernel/sys_fs.c` with a comment saying why "not supported" is the honest
answer. systest: `fsetxattr` on an open file gives `-EOPNOTSUPP` (95), `flistxattr` gives
0, `getxattr` on a missing path gives `-ENOENT` (2). One commit, full list.

(Also in that log: 430, `fsopen`, from util-linux `mount`. Leave it: `mount` falls back to
the old `mount()` call when the new API answers `-ENOSYS`. Claude will check that it does.)

---

## 7. Reserved for Claude — don't start these

So we don't collide. If you finish everything above, write "done" in the notes and stop.

- The x86_64 live desktop (`make ARCH=x86_64 live-uefi`): dynamic linking, X, IceWM.
  Don't build or edit the live images or `live/`; Cursor and Claude work there.
- (done) evdev's `struct input_event` on x86_64; `mincore` (it was a stub).
- `copy_file_range` (T08) and the xattr calls (T12).
- `madvise` (a stub today: MADV_DONTNEED must really drop anonymous pages).
- 64-bit lengths in `mm/mm.c` (munmap/mprotect/mremap cut lengths to 32 bits on x86_64).
- Mapping a framebuffer above 4 GiB for programs (`vma->phys` is 32-bit).
- `timerfd_*`, `signalfd`, `sigaltstack`, `waitid`, `clone3` — they reach into timers,
  signal delivery and process management.
- Real-musl tests on x86_64 (needs a download Rodger hasn't approved yet).
