# Sleep, as on a Mac

What was asked for (2026-10-22): closing the lid puts the laptop to sleep,
opening it wakes it to the lock screen, as on a Mac. Built on its own branch
(`sleep`, worktree `../MyOS-sleep`) so other work on main goes on, tested in
QEMU as closely as QEMU allows, and aimed at the GPD P2 Max first. Decided
along the way:

- **Suspend to idle first** (Linux's "s2idle", Intel's S0ix): the machine
  stays in S0, programs stop, the CPU idles with the tick off until something
  wakes it. Every device stays powered, so the USB stick we run from, the
  Wi-Fi card's firmware and the picture all survive, with no device having to
  learn to resume. S3 (suspend to RAM: the CPU off, the firmware restarting
  it at a wake vector) is phase 2.
- **The kernel only sleeps; a program decides when.** As on Linux: the lid
  and the power button are `/dev/input` devices, and writing `mem` to
  `/sys/power/state` sleeps until something wakes it. The live system's
  `myos-powerd` reads the lid and the button and asks the session to lock
  first.
- **The lock is up before the machine sleeps**, so waking never shows the
  desktop, not even for a frame.

## How it fits together

```
lid shut: EC query -> Notify(LID0, 0x80)    power button: fixed event or PNP0C0C
        │                                           │
        ▼                                           ▼
drivers/acpi_button.c   _LID; the boot log of what the firmware offers
        │  SW_LID on /dev/input/event3 ("Lid Switch")
        │  KEY_POWER on /dev/input/event4 ("Power Button"), KEY_WAKEUP if it woke us
        ▼
myos-powerd (root, runit)   /etc/myos/power.conf: what the lid and the button do
        │  Start menu's Sleep and myos-idle write "sleep" to /run/myos/power
        │  SIGUSR1 ──► myos-idle ──► myos-lock --ack ──► "locked" on /run/myos/power
        │  (waits up to 1 s for it)
        ▼
echo mem > /sys/power/state          kernel/suspend.c
        │  1. programs stop at their next return to user mode (the freezer)
        │  2. the disks' changed blocks written
        │  3. the screen (drm) and the backlight off
        │  4. every 8259 line off but those that wake; Linux drivers' MSIs held
        │  5. the idle thread stops the 1 ms tick (thread.c, pit.c / hpet.c)
        ▼
asleep: the CPU halts until the lid opens, the power button, or a key or a click
        │  (a key only with the lid open: a shut lid can press keys)
        ▼
the same steps backwards; the wall clock and BOOTTIME jump by the time asleep
        │
        ▼
myos-powerd: SIGUSR2 ──► myos-idle: the screen saver reset, myos-lock redraws its clock
```

## The pieces

| Piece | Where | State |
|---|---|---|
| The firmware's sleep facts in the boot log: s2idle (the FADT's low-power S0 idle flag), `\_S3_`, the lid and its `_PRW` wake GPE, a fixed or a device power button | `drivers/acpi_button.c` | done; the P2 Max's answers wait for a stick |
| The lid (PNP0C0D, `_LID`, notify 0x80; a `_PRW` GPE with no AML of its own turned on as an implicit notify) | `drivers/acpi_button.c` | done |
| The power button, fixed (PM1) or PNP0C0C | `drivers/acpi_button.c`, `drivers/acpi_os.c` | done |
| "Lid Switch" (EV_SW, `EVIOCGSW`) and "Power Button" (EV_KEY) as event3 and event4, with sysfs uevents | `drivers/evdev.c`, `fs/dev.c`, `fs/sysfs.c` | done |
| `/sys/power/state` (`freeze mem`), `/sys/power/mem_sleep` (`[s2idle]`) | `fs/sysfs.c`, `kernel/suspend.c` | done |
| The freezer: one flag at the user-mode edge (`signal_check`) | `kernel/signal.c`, `kernel/suspend.c` | done |
| Wake-up lines only: the 8259 masks, Linux drivers' MSIs held and replayed | `arch/*/pic.c`, `kernel/linux_irq.c` | done |
| The tick off while asleep (PIT or HPET), back on at any interrupt | `kernel/thread.c`, `drivers/pit.c`, `drivers/hpet.c` | done |
| The clock jump: REALTIME and BOOTTIME, by the TSC, overruled by the CMOS clock when they differ by over 2 s | `kernel/sys_info.c`, `kernel/suspend.c` | done |
| `myos-sleepcheck`: did the lid really sleep? A line a second to `~/sleep-check.txt`; a gap with the uptime standing still is a sleep | `live/overlay/usr/bin/myos-sleepcheck` | done |
| The screen off and on again | `drivers/gpu/drm/drm_core.c`, `drivers/backlight.c` | done |
| A key or a click wakes (with the lid open), and isn't typed | `drivers/keyboard.c`, `drivers/evdev.c`, `drivers/usb/xhci.c` | done |
| `myos-powerd`: the lid, the button, the FIFO; the lock handshake | `live/myos-powerd`, `live/overlay/etc/sv/myos-powerd` | done |
| `/etc/myos/power.conf` | `live/overlay/etc/myos/power.conf`, `live/common/powerconf.h` | done |
| `myos-lock`: the lock screen, in the login screen's look; PAM (`/etc/pam.d/myos-lock`) | `live/myos-lock` | done |
| `myos-idle`: locks when the screen goes off; on battery, sleeps a minute later | `live/myos-idle` | done |
| Sleep in the Start menu, with its moon icon | `live/theme/make-live-theme.py`, `live/assets/themes/golden-hour/make-theme.py` | done |
| A lid for QEMU: `tools/fakelid.py`'s SSDT, moved by a CPU added in QEMU's monitor (SLEEPTEST builds only) | `tools/fakelid.py`, `kernel/fakelid_test.c` | done |
| `make test-sleep` (both kernels), `make ARCH=x86_64 SLEEPTEST=1 test-sleep-desktop` | `tools/test-sleep.py`, `live/tools/sleep-desktop.qs` | done |
| The P2 Max: the boot log, then sleeping and waking from the lid | a stick, `tools/stick-logs.sh` | next |

## The settings

`/etc/myos/power.conf`, read again at each event (an edit needs no restart);
the keys are `docs/power-performance-settings.md`'s:

| Key | Values | Default |
|---|---|---|
| `lid_close` | `sleep`, `display_off`, `nothing` | `sleep` |
| `power_button` | `sleep`, `poweroff`, `ignore` | `sleep` |
| `lock_on_wake` | 1, 0 | 1 |
| `lock_on_display_off` | 1, 0 | 1 |
| `sleep_after_display_off_minutes` | minutes on battery, 0 never | 1 |

Plugged in, it never sleeps by itself. An account without a password gets
"Press Enter or click to unlock" on the lock screen (PAM is asked with an
empty password, without its failure delay; `base-auth` has no lockout).

## Testing in QEMU

QEMU has no lid. `tools/fakelid.py` writes an SSDT with `\_SB.LID0`
(PNP0C0D, `_LID`, `_PRW`, and `TOGL`, which flips it and notifies). In a
`SLEEPTEST=1` build (a build folder of its own), `kernel/fakelid_test.c`
listens on QEMU's processor objects: `device_add` of a CPU in QEMU's
monitor (with `-smp 1,maxcpus=4`) raises GPE 2, QEMU's own AML notifies the
processor, and the handler runs `TOGL`. So the lid moves through a real
GPE, a real interrupt and real AML, as the P2 Max's embedded controller
does it. (The first plan used the vmgenid device; QEMU 11 refuses to change
its guid once it runs.)

- `make test-sleep`, i386 by BIOS and x86_64 by UEFI: the host plays
  `myos-powerd`. The lid shuts, a busy loop stops (QEMU's CPU time falls to
  0%), a key with the lid shut leaves it asleep, the lid opens it, the
  wall clock jumped and the uptime didn't, and the kernel's time asleep is
  the Mac's within 1.5 s; a key and the power button wake it; event3 and
  event4 report what happened.
- `make ARCH=x86_64 SLEEPTEST=1 test-sleep-desktop`: the whole live
  desktop. The lid, the Start menu's Sleep and the power button each put it
  to sleep behind the lock screen; it is unlocked after each wake, first
  with no password, then with a wrong one and the right one, and
  `myos-sleepcheck` must find the lid's 8 s asleep. Screenshots in
  `build/x86_64/sleeptest/sleep-*.png`.

## On the P2 Max (next)

1. Boot this branch's stick, run `myos-sleepcheck` in a terminal, close
   the lid for a minute, open it and unlock. "ASLEEP for about N s" with
   the uptime nearly still is a real sleep; lines going on every second
   mean the lid only turned the screen off; a gap with the uptime moving
   means it hung. Under it, the kernel's line: what woke it, the time
   asleep by the CMOS clock and by the TSC, and the time to wake. Compare
   the clock with a phone too.
2. Read the logs back on the Mac with `tools/stick-logs.sh` (the bootlog
   service saves the kernel log every minute). The boot log's `acpi:
   sleep:`, `acpi: lid:` and `acpi: power button:` lines say what the
   firmware offers; `lid: shut` / `lid: open` say whether the lid reaches us.
3. Wake time, and the battery over 30 minutes asleep against 30 awake on
   the desktop (`/sys/class/power_supply/BAT0/energy_now`), into
   `docs/performance.md`.
4. If the lid never notifies: its `_PRW` GPE and the embedded controller's
   queries in the boot log, then the DSDT (`/sys/firmware/acpi/tables`).

## Phase 2: S3, deep sleep

Only where the firmware offers `\_S3_` (the boot log says) and s2idle drains
the battery too fast:

- [ ] the wake vector in the FACS: real mode back to long mode (or
  protected mode), the CPU's state restored (GDT, IDT, page tables, MSRs,
  the TSC's jump)
- [ ] `_PTS`, `_WAK`, the SLP_TYP and SLP_EN write (uACPI's
  `uacpi_prepare_for_sleep_state` and `uacpi_enter_sleep_state`)
- [ ] every device back from D3: xHCI (the stick we run from), AHCI and
  NVMe, the 8259 and the HPET, iwlwifi through the shim, the display
  (no GPU driver: the firmware's framebuffer may not come back)
- [ ] a dark wake: a battery event wakes it, it looks, and sleeps again
- [ ] later: hibernate when the battery is nearly empty (the memory to the
  disk), only if a user asks for it
