# The battery in the tray, and the power-saving slider

What was asked for (2026-10-21): the battery in the tray, with a
power-saving slider, the time until it is fully charged, the time until it
is empty, and the battery's health. Decided along the way:

- **Read the battery through a real ACPI interpreter**, not one laptop's
  embedded-controller addresses, so it works on every laptop (P2 Max, MSI,
  Pocket 4, old Pentium M machines) and also opens the way to lid, sleep,
  backlight, the I2C touchpad's tables and the processor's idle and speed
  tables.
- **The slider has three steps**: Best battery life, Balanced, Best
  performance — Linux's platform profiles. It changes the processor at
  once, no restart.
- **uACPI 6.1.1** is the interpreter (MIT licence, `third_party/uacpi`).

## How it fits together

```
firmware tables (DSDT, SSDTs)          drivers/acpi.c   finds and maps them
        │                                               /sys/firmware/acpi/tables
        ▼
uACPI, in the kernel thread "acpi"     drivers/acpi_os.c  the uacpi_kernel_* glue
        │  runs _BIX/_BIF, _BST, _PSR, _STA              SCI on IRQ 9, GPEs
        │  reads the embedded controller drivers/acpi_ec.c   ports 0x62/0x66
        ▼
drivers/battery.c   every 10 s, and on Notify(0x80/0x81): a copy of the numbers
        │
        ▼
/sys/class/power_supply/BAT0/…  AC/online     fs/sysfs.c (O(1): shows the copy)
/sys/firmware/acpi/platform_profile           arch/*/cpupower.c
        │
        ▼
myos-menu   the icon in the tray, filled to the charge (live/myos-menu)
myos-battery  the pop-up: charge, time left, health, the slider (live/myos-battery)
```

No system call ever waits for the firmware: AML can take milliseconds (the
embedded controller answers a byte at a time), so it runs only in the acpi
thread, and `/sys` reads the copy it keeps.

## The pieces

| Piece | Where | State |
|---|---|---|
| ACPI tables in `/sys/firmware/acpi/tables` (root only) | `drivers/acpi.c`, `fs/sysfs.c` | done |
| uACPI and its kernel glue; the SCI | `third_party/uacpi`, `drivers/acpi_os.c` | done, both kernels |
| Embedded controller | `drivers/acpi_ec.c` | done; only real laptops have one |
| Battery and charger, Linux's files and units (µWh, µW, µV; µAh and µA for a battery that counts in mAh) | `drivers/battery.c` | done |
| Platform profile: low-power / balanced / performance | `arch/*/cpupower.c`, `fs/sysfs.c` | done |
| The profile writable by group wheel | `live/overlay/etc/runit/1` | done |
| The tray icon | `live/myos-menu`, `live/theme/make-live-theme.py` | done |
| The pop-up | `live/myos-battery` | done |
| The panel's backlight (`_BCL`/`_BCM`/`_BQC`) as `/sys/class/backlight/acpi_video0`, writable by group video | `drivers/backlight.c`, `fs/sysfs.c`, `live/overlay/etc/runit/1` | done, both kernels |
| Dim, then the screen off, when idle; sooner along the slider | `live/myos-idle` | done |

### What the slider does

| Step | `platform_profile` | The processor |
|---|---|---|
| Best battery life | `low-power` | boost off, at most 60% of its rated speed, energy preference "energy"; without HWP/CPPC, clock modulation to half speed |
| Balanced | `balanced` | the whole range, boost allowed, the preference in between (the boot default: the kernel config's settings stand) |
| Best performance | `performance` | the whole range, boost allowed, the preference "speed" |

The slider also sets how long the screen stays on without a key or a
mouse move (`live/myos-idle`, asked for 2026-10-21):

| Step | On battery | Plugged in, or no battery |
|---|---|---|
| Best battery life | off after 1 min | 2 min |
| Balanced | 5 min | 10 min |
| Best performance | 15 min | 30 min |

20 seconds before it goes off the screen dims to 30% (the backlight; a
half-dark window over everything where there is no backlight but a
compositor), and any key or move brings it back. Off is the backlight's
lowest level, X's blank screen and DPMS off. myos-idle sleeps on X's
SYNC idle-time alarms and reads the slider and the charger only when an
alarm fires, so moving the slider needs no message to it.

Any program can read it; administrators (group wheel) can write it. For
other users the pop-up shows the setting and says only an administrator
can change it.

### What the pop-up shows

The card is `live/assets/themes/golden-hour/battery-popup.png`: it sits
above the bottom tray, not a Mac-style menu bar.

- **Charge**: a large percent.
- **Time**: beside it, "2 hr 14 min left" while discharging, or "1 hr 20
  min to full" while charging (`energy_now / power_now`, or what is
  missing over the rate). Until the firmware has measured a rate it says
  "Working out the time…".
- **Plugged in**: a line under the percent: "On battery", "Plugged in ·
  charging", "Plugged in · fully charged", or "Plugged in · not charging".
- **Health**: a short bar and "Health  Good  92%": what it holds when
  full now over what it held new (`energy_full / energy_full_design`).
  80% or more is good, 60–79% fair, below that worn. Plus the charge
  cycles when the firmware counts them.
- **Slider**: Lifespan / Balanced / Speed (the three platform profiles).

The icon is filled white, green while on the charger, red at 10% or less,
and isn't drawn at all on a machine without a battery.

## Tests

- `make test-battery` (i386 by BIOS, and `ARCH=x86_64` by UEFI): QEMU has
  no battery, so `tools/fakebat.py` writes an SSDT with one (50 Wh new,
  45 Wh now, 9 W out, 0.1 Wh less each read) and an unplugged charger. It
  checks the interpreter starts, every `/sys` file and unit, that the
  numbers move with the 10-second reading, and that the profile switches
  and can be handed to wheel. The SSDT also has a panel (`\_SB.GFX0.LCD`,
  `_BCL` out of order with a repeat): 11 levels, 5% to 100%, and a write
  of 3 to `brightness` reaches `_BCM` and comes back from `_BQC`.
- `live/tools/idle-desktop.qs` (the live image, by `live/tools/qemu-desktop.py`
  with the fake battery): Best battery life on battery is full at 30 s,
  dimmed (3 of 10) at 45 s, off at 65 s, back at a mouse move; Balanced
  hasn't dimmed at 50 s; back on Best battery life it dims at 45 s again.
- `make test-power-supply`: the tray's reading of `/sys` on the build
  machine, with made-up files (energy and charge batteries, charging,
  discharging, no rate yet).

## Still to do

- **Try it on the P2 Max and the MSI.** QEMU has no embedded controller;
  real firmware is where the EC handshake, the GPEs and `_Qxx` news get
  tested. If the battery doesn't show, `/var/log/boot.log` has the acpi
  lines, and `/sys/firmware/acpi/tables/DSDT` can be copied off.
- **Old PCs' speed steps.** On a Pentium M the low-power step uses clock
  modulation; the right way is SpeedStep's lower voltage and frequency
  pairs, from ACPI's `_PSS`, which the interpreter can now read.
- **Low-battery action** (sleep or power off at N%) and the On battery /
  Plugged in profiles in `docs/power-performance-settings.md`: the numbers
  they need are here now.
- **Lid and sleep** through the same interpreter. Brightness keys and a
  brightness slider (the backlight is there now).
