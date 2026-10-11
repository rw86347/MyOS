# Time slots: who runs when

Status: **design**. Today's kernel has the 20-slot frame (`kernel/sched.c`)
and gives a slot to whichever thread has the right name. This document is
how it is meant to work:
- **Part 1:** how a program is bound to its slots;
- **Part 2:** how the kernel and the desktop share the two OS slots, so a
  runaway program can never freeze the screen;
- **Part 3:** what a program written for a slot looks like.

The settings themselves (the mode, the slot names, the programs) and where
they are kept are in `docs/KERNEL-CONFIG.md`.

## The frame

```
 slot:  0    1    2    3    4    5    6    7    8    9   10   11  ...  19
       [OS ][   ][aud][   ][brk][   ][rad][   ][   ][brk][OS ][   ] ... [brk]
        └─ 1 ms ─┘                                         └ the frame repeats every 20 ms
```

- 20 slots of 1 ms, numbered 0–19, repeating every 20 ms.
- **Slots 0 and 10 belong to the OS**: exactly every 10 ms, whatever else
  is going on.
- The other 18 are either a **task's** (named in Settings › Kernel) or
  **free**.

Every thread is one of three kinds:

| Kind | Who | How it gets that way |
|---|---|---|
| **Task** | A program named for some slots, such as `/bin/brakes` in 4, 9, 14 and 19 | Started by `myos-taskd` and bound at `execve` (Part 1) |
| **OS** | The kernel's own threads, and the desktop's own programs: the X server, the window manager and taskbar, the Start menu, Force Quit, the login screen | Kernel threads always; programs by running a file on the OS list (Part 2) |
| **Ordinary** | Everything else: the shell, the browser, the office programs, a runaway loop | The default |

## Who runs in each slot

| Slot | Realtime | Soft realtime | Max power savings |
|---|---|---|---|
| **0, 10 (OS)** | OS threads only. If none is ready, ordinary programs use the rest, until an OS thread wakes and takes it back at once | the same | no slots: OS first, then ordinary (below) |
| **A task's slot** | The task, from the slot's start to its end. If it is asleep or gave the slot away: OS first, then ordinary. A waking OS thread waits for the slot's end | The task first. A waking OS thread may cut in | no slots |
| **A free slot** | OS first, then ordinary programs, round robin, 1 ms each | the same | no slots |
| **Any time** | Interrupts run when they arrive (the minimum: acknowledge, copy, wake). Their remaining work runs in kernel threads, which are OS threads | the same | the same |

Three rules make that table:
1. **OS before ordinary, everywhere.** When an OS thread becomes ready, it
   runs **at once**, ahead of any ordinary program. The kernel doesn't
   wait for the next tick: the interrupt that woke the thread switches to
   it on its way out.
2. **A task owns its slot** (realtime), or is first in it (soft realtime).
3. **The OS slots are the floor.** Even if tasks fill every other slot,
   the OS runs at least 2 ms of every 20 ms, at most 10 ms apart.

## Part 1 — binding a program to its slots

### The steps, for `brakes` → `/bin/brakes` in slots 4, 9, 14, 19

1. **At boot**, the kernel reads the config (`docs/KERNEL-CONFIG.md` §8).
   - From the boot block it now knows: slots 4, 9, 14, 19 belong to task
     number 2, called `brakes`.
   - From the task list it knows: `brakes` is `/bin/brakes --hz 200`, runs
     as `nobody`, and starts at boot.
   - Nothing is started yet, because `/bin` isn't mounted.
2. **Until `brakes` runs**, its slots are reserved but empty. Ordinary
   programs use them (leftover time), so a missing task costs nothing.
3. **After the root file system is mounted**, runit starts `myos-taskd`
   first, before the login screen. It reads the task list the kernel
   keeps (`/sys/kernel/myos/tasks`). Then, for each task that starts at
   boot, it:
   1. forks;
   2. in the child, asks the kernel "the next program this process runs
      is task `brakes`". Only root may ask, and `myos-taskd` is root;
   3. switches the child to the task's user and group (`nobody`);
   4. runs `/bin/brakes --hz 200`.
4. **At `execve`, the kernel binds it**, only if all of these hold:
   - the process was marked for `brakes` by root;
   - the file being run **is** `/bin/brakes`: the same file, compared by
     its inode, not by its name;
   - the process runs as the task's user;
   - no living process is `brakes` already.

   If any check fails, `execve` fails with `EPERM`, and `myos-taskd` logs
   why. When all hold, the process is the task from the next slot boundary.
5. **The whole process is the task**: its threads share its slots. A
   child it forks is an ordinary program. That way a task can't hand its
   slots to something it started.
6. **When it exits**, its slots are empty again. With "restart if it
   stops" on, `myos-taskd` waits the restart delay and goes back to
   step 3.

### What a name no longer does

Today `sched.c` binds by **name**: `thread_find("brakes")`. Any program
can call `prctl(PR_SET_NAME, "brakes")`, so any user could take `brakes`'s
slots. Under the new rule a name gives nothing; only the steps above bind
a program.

### Seeing it work

`/proc/myos/slots` (to come) shows one line per slot. For example, slot
4 is `brakes`, pid 312, with its overruns and its worst lateness.
Settings › Kernel shows each task as running or not started. `make test`
gets cases for a renamed thread (no slot), a correct start (its slots), a
second copy of the same program (`EPERM`), and a different file under the
right name (`EPERM`).

## Part 2 — the OS slots: the desktop always answers

### The goal

Rodger's words: a runaway process must never stop the GUI. It should
always be possible to open the Start menu, start Activity Monitor (or
Force Quit, Ctrl+Shift+Esc), and kill the runaway, **with no lag**.

**Today it can lag.** Ordinary programs share the processor round robin,
1 ms each, and the X server is one of them. With four runaway loops, X
gets about a fifth of the processor, and only when its turn comes round.
Slots 1 and 10 run only the kernel's `main` thread.

### Who is "the OS"

1. **The kernel's own threads**, always: `main`, the USB thread, the
   network and disk helpers, `idle`.
2. **The desktop's own programs**, by file. The **OS list** holds them,
   for example:

   | Program | Why it must never wait |
   |---|---|
   | `/usr/libexec/Xorg` | every key press, mouse move and window drawing goes through it |
   | `/usr/bin/icewm` | the window manager and the taskbar; the keyboard shortcuts (Ctrl+Shift+Esc) |
   | `/usr/bin/myos-menu` | the Start menu |
   | `/usr/bin/myos-forcequit` | Force Quit (and Activity Monitor, when it exists) |
   | `/usr/bin/myos-desktop` | the desktop icons |
   | `/usr/bin/myos-login` | the login screen and the session's start |
   | `/usr/sbin/myos-taskd` | starts and restarts the tasks |

   The list comes with MyOS and is root's to change: Settings shows it,
   read-only in v1. It is saved with the task list (that part of the disk
   format grows from 5 to 8 KiB before anything reads it).

### How a program joins

- **At `execve`**: a process that runs a file on the OS list becomes an
  OS thread. Running any other file makes it ordinary. A forked child
  keeps its parent's kind until it runs something. Files are compared by
  inode, like tasks. Every file on the list must be owned by root and
  writable by no one else; Save checks this, and so does the kernel.
- So the Start menu that icewm starts is an OS thread, but the browser
  the Start menu starts is not: it runs `/usr/bin/firefox`, which isn't on
  the list.
- **Anyone may start a listed program.** You need Force Quit most when
  something has gone wrong. Abuse is bounded by the next rule.

### The fence around the OS class

An OS thread that **keeps the processor**, using more than 300 ms of
every second, is moved to ordinary for 2 seconds, and the kernel logs
it. Then:
- a runaway X server, or someone running a listed program with a bad
  `LD_PRELOAD`, can take at most a bounded share;
- the rest of the OS class (Force Quit above all) still gets its time,
  round robin among OS threads;
- normal desktop work, which wakes, draws and sleeps, is never anywhere
  near the fence.

### The path from a click to a killed runaway, and its worst wait

1. **The mouse moves.** Its interrupt wakes the X server (OS). Because OS
   comes before ordinary, X runs as the interrupt returns. The wait is a
   few microseconds, not "until the runaway's millisecond is over".
2. **X wakes icewm** (OS), which shows the Start menu or, on
   Ctrl+Shift+Esc, starts Force Quit (OS: it's on the list).
3. **Starting Force Quit** reads its file from the disk. A runaway that
   hammers the disk could slow that down, so:
   - **Force Quit stays loaded**: started hidden at login, the shortcut
     only shows it;
   - **OS threads' disk reads go to the head of the queue.**
4. **Kill**: Force Quit sends SIGKILL. The kernel delivers it the next
   time the runaway enters the kernel or is interrupted (the 1 ms tick at
   the latest) and frees its memory.

| Mode | Worst wait before the GUI runs |
|---|---|
| No tasks, or soft realtime, or power savings | Microseconds: OS threads cut in at once |
| Realtime with tasks | The longest run of task slots in a row. With slots 1–9 all tasks, that's 9 ms, under one 60 Hz frame (16.7 ms) |

**Memory runaways:** MyOS has no swap, so nothing of the desktop is
paged out. When memory runs out, the out-of-memory killer never picks an
OS thread; it picks the biggest ordinary program.

**Interrupt work:** the kernel threads that finish an interrupt's work
(network, USB, disk) are OS threads. They keep running ahead of a
runaway, so the network and the USB keyboard keep working too.

### What the kernel needs for this (steps)

1. **A kind per thread** (task / OS / ordinary). `sched_pick` uses it
   (Part 2, rule 1), and kernel threads start as OS.
2. **Preempt on wake-up:** when an interrupt wakes an OS thread while an
   ordinary one runs, switch on the way out. `sched_preempt_check()` is
   already called there; it needs the reason.
3. **The OS list in the config**, and joining at `execve`.
4. **The fence**: CPU time per OS thread over the last second.
5. **I/O priority** for OS threads in the disk queue.
6. **The out-of-memory rule.**
7. **Measure it.** Run four `while(1);` loops and one disk hog, inject a
   key press (QEMU `sendkey`), and time it until X receives the event.
   Before and after go in `docs/performance.md`.

## Part 3 — writing a program for a slot

A task is an ordinary Linux program (musl, static or dynamic). Its main
loop does one round of work, then gives the rest of its slot away:

```c
/* /bin/brakes — runs in slots 4, 9, 14 and 19: every 5 ms. */
#include <sched.h>

int main(void)
{
    setup();                        /* open devices, allocate everything now: */
                                    /* no allocation, no file opening in the loop */
    for (;;) {
        read_sensors();
        decide();
        drive_outputs();
        sched_yield();              /* done for this slot: the rest goes to others; */
    }                               /* the kernel wakes us at our next slot */
}
```

- **`sched_yield()` from a task means "I'm done with this slot".** The
  kernel doesn't pick the task again until its next slot starts, so the
  loop runs once per slot, at the slot's start.
- **Keep the round shorter than the slot** (1 ms; less on a slow PC). In
  realtime an overrun is cut off at the slot's end and counted; in soft
  realtime it may run over a little.
- **Don't wait on the disk or the network in the loop.** Those waits
  can't be promised. Read ahead in `setup()`, or let a helper thread
  (ordinary, in free time) feed the task through memory.
- **Do the slow work outside the loop**: allocate, open and load
  everything in `setup()`. Page faults and allocation in the loop add
  jitter.
- **The time:** `clock_gettime(CLOCK_MONOTONIC)` at the top of the loop
  says how late the round started. Later, the vDSO makes that call cost
  no system call at all.

## How today's kernel differs

| Today (`kernel/sched.c`) | Planned |
|---|---|
| Slots 1–20, system slots 1 and 10 (9 ms then 11 ms apart) | Slots 0–19, OS at 0 and 10 (exactly 10 ms apart) |
| A slot goes to whichever thread has the name | Bound by `myos-taskd` and checked at `execve` |
| The system slots run only `main` | They run the whole OS class: the kernel's threads and the desktop |
| Ordinary programs and the X server share time equally | OS threads first, everywhere; the fence stops a runaway among them |
| One table, built in (`config/slots.conf`) | The config on the disk; the built-in table is only the default |
