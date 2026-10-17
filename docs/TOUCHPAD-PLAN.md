# Touchpad plan: two-finger scrolling and palm rejection

Status: **plan** (2026-10-10). WORKLIST B52. Nothing below exists yet.

## 1. The goal

A touchpad that feels like a Mac's. In order of importance:
1. **Two-finger scrolling**: smooth, with the content following the
   fingers ("natural" scrolling).
2. **Palm rejection**: a palm resting on the pad while typing does
   nothing.
3. Tap to click, two-finger click (or tap) as a right-click, three-finger
   drag.
4. Later: pinch and swipe gestures.

The first machine is the MSI (`docs/hardware-msi-katana-gf66.md`). The
others are in §11.

## 2. Where MyOS is today

- **The only pointer is the PS/2 mouse** (`drivers/mouse.c`). It reaches
  `/dev/input/event1` as plain movement (`REL_X`, `REL_Y`, `REL_WHEEL`),
  and X reads it with `xf86-input-evdev`.
- **The MSI's touchpad isn't on the PS/2 port.** It is an I2C HID device
  (ACPI `PNP0C50`) on the chipset's Intel LPSS I2C controller `8086:43e8`.
  The chip is a Synaptics `06CB:7E7E` or an ELAN `04F3:30AA`. MyOS can't
  see it at all.
- **No ACPI tables are read.** `drivers/hpet.c` guesses the HPET's
  address, and `drivers/power.c` can power off only in QEMU, because the
  numbers are in tables (and AML) MyOS doesn't read. The touchpad's
  address and interrupt pin are there too.
- **No I2C, no GPIO, no HID.**

## 3. The design: raw fingers from the kernel, the feel from libinput

```
 touchpad ─I2C─▶ LPSS I2C ─▶ i2c-hid ─▶ HID parser ─▶ /dev/input/event2 ─▶ libinput (in X) ─▶ programs
 (Synaptics/      controller   (HID over   (precision    multitouch:          two-finger scroll,   smooth
  ELAN chip)      (DesignWare)  I2C)        touchpad)     every finger,        palms, tapping,      scrolling
       │                                                  palms marked         typing
       └─ interrupt: a GPIO pin (polled every 8 ms at first)
```

The kernel's job ends at **every finger's position, with palms marked**,
on a standard Linux multitouch device. Turning fingers into scrolling,
clicks and palm decisions is **libinput**'s job: the library every Linux
desktop uses, run unmodified (MyOS's second goal).

Why not do the gestures in the kernel? It would be smaller: `evdev.c`
could turn two fingers into wheel clicks, and X's current driver would
work unchanged. But:
- scrolling would come in notches, with no smoothness or inertia;
- every rule about palms, typing and tapping would be ours to invent and
  tune;
- it would put policy in the kernel.

libinput is years of tuning on thousands of touchpads.

## 4. Step 1: the ACPI tables, and what they say about the pad

**Find them.**
- On UEFI, the loader passes the RSDP's address in `bootinfo`. It is in
  the EFI configuration table, under the ACPI 2.0 GUID
  `8868E871-E4F1-11D3-BC22-0080C73C8881`.
- On BIOS, the kernel searches for `"RSD PTR "` in `0xE0000–0xFFFFF`, on
  16-byte boundaries.
- Then it walks the XSDT (or RSDT), checking each table's checksum.

**Show them**, as Linux does: `/sys/firmware/acpi/tables/<signature>`
(`DSDT`, `SSDT1`…, `FACP`, `APIC`, `HPET`, …). These files are readable
**by root only**: some tables hold secrets, such as the Windows product
key in `MSDM`.

**Read the MSI's.** Over ssh, copy `DSDT` and the `SSDT`s to the Mac and
decompile them with Intel's `iasl -d` (`brew install acpica`, asked
first). Find the device whose `_HID` or `_CID` is `PNP0C50`, and write
down:
- `I2cSerialBusV2`: the pad's **address**, its **bus speed**, and
  **which controller** it hangs on (`\_SB.PC00.I2Cn`);
- `GpioInt(...)` (or `Interrupt(...)`): the **pin**, and whether it is
  level or edge, active high or low;
- `_DSM` function 1 (GUID `3CDFF6F7-4267-4555-AD05-B30A3D8938DE`): the
  **HID descriptor register**;
- the controller's `SSCN` / `FMCN` methods: its **bus timings**.

These go into `docs/hardware-msi-katana-gf66.md`, and into a small
per-machine table in the kernel:

```c
/* Where a touchpad is, until MyOS can run ACPI's AML itself. */
struct i2c_hid_board {
    const char *oem_table_id;       /* matched against the DSDT's header */
    uint16_t i2c_pci_device;        /* 0x43E8: LPSS I2C controller #0 */
    uint8_t addr;                   /* the pad's 7-bit I2C address */
    uint16_t hid_desc_reg;          /* from _DSM function 1 */
    uint16_t gpio_pin;              /* its interrupt line */
    uint8_t level_low;
};
```

The tables pay for themselves beyond the touchpad:
- the HPET's real address (`hpet.c`);
- the MADT for the APICs;
- the FADT for power off.

**A real AML interpreter** (porting ACPICA, or writing a subset) is a
roadmap item of its own. It would replace the per-machine table, and
also bring power off, sleep, the lid switch, the battery and the
backlight to every laptop.

## 5. Step 2: the I2C controller (Intel LPSS, DesignWare core)

- **The device:** PCI `8086:43E8` (Tiger Lake-H Serial IO I2C). MyOS
  already finds PCI devices behind bridges (`drivers/pci.c`).
- **Power it up:** PCI power state D0, then take it out of reset through
  the LPSS private registers at `BAR0 + 0x200`. The exact bits are in
  Linux's `drivers/mfd/intel-lpss.c`; its input clock per chipset is in
  `intel-lpss-pci.c`.
- **The I2C core** at `BAR0` is Synopsys DesignWare. It identifies
  itself: `IC_COMP_TYPE` (`0xFC`) reads `0x44570140`. Its registers:
  - `IC_CON`: controller mode and speed;
  - `IC_TAR`: the target address;
  - `IC_DATA_CMD`: one byte per write; bit 8 means read, bit 9 STOP, bit
    10 RESTART;
  - `IC_*_SCL_HCNT/LCNT`: the bus timings, from `SSCN`/`FMCN`;
  - `IC_ENABLE`, `IC_STATUS`, `IC_INTR_*`.
- **Transfers are interrupt-driven from the start**: RX full, TX empty,
  stop, abort. A 30-byte read at 400 kHz takes almost a millisecond,
  which is too long to busy-wait (CLAUDE.md, rule 1).
- **Reused later:** the GPD Pocket 4's I2C (`AMDI0010`) is the same
  DesignWare core.
- About 400 lines (`drivers/i2c/designware.c`).

**Visible result:** the boot log prints the pad's HID descriptor,
including `06CB:7E7E` or `04F3:30AA`.

## 6. Step 3: HID over I2C, and the precision-touchpad reports

**HID over I2C** (Microsoft's spec; Linux `drivers/hid/i2c-hid/i2c-hid-core.c`):
- Read the 30-byte HID descriptor at the register from step 1. It gives:
  - the report descriptor's length and register;
  - the input register and the longest input report;
  - the command and data registers;
  - the vendor and product IDs.
- Commands go through the command register: `RESET`, `SET_POWER` (on),
  and `GET_REPORT` / `SET_REPORT` for feature reports.

**The report descriptor parser**: general HID, so USB keyboards and mice
(B50) and the Pocket 4's touchscreen use it too. A first one exists
(`drivers/hid/hid.c`, 2026-10-17, for USB keyboards and mice): it keeps
keys, buttons and relative X/Y/wheel; this step adds the digitizer page,
absolute values with their physical size, and feature reports.
- It walks the items (Usage Page, Usage, Logical Min/Max, Physical
  Min/Max and Unit, Report Size/Count, Report ID, Input/Output/Feature,
  Collection/End Collection).
- The result: for each report ID, where each usage's bits are. About 600
  lines (`drivers/hid/hid.c`).

**The precision-touchpad usages** (Microsoft's "Windows Precision
Touchpad" spec):

| Usage | Page:ID | Use |
|---|---|---|
| Touch Pad (collection) | Digitizer `0x0D:0x05` | this is a touchpad |
| Finger (logical collection) | `0x0D:0x22` | one per contact |
| Tip Switch | `0x0D:0x42` | the finger is down |
| **Confidence** | `0x0D:0x47` | **0 = a palm or an accidental touch** |
| Contact Identifier | `0x0D:0x51` | follows one finger from touch to lift |
| X, Y | Generic Desktop `0x01:0x30/0x31` | position, with its physical size: real millimetres |
| Contact Count | `0x0D:0x54` | fingers in this frame |
| Scan Time | `0x0D:0x56` | the pad's own timestamp, in 100 µs units |
| Button 1 | Button `0x09:0x01` | the click (it's a clickpad: the whole pad presses) |
| Contact Count Maximum (feature) | `0x0D:0x55` | how many fingers it tracks |
| **Input Mode** (feature) | `0x0D:0x52` | write **3** to switch from mouse mode to finger reports |
| Surface / Button Switch (feature) | `0x0D:0x57/0x58` | write 1 to report both |

Things to handle:
- **Hybrid reporting:** some pads send all five fingers in one report,
  others two per report, with the count in the first. The parser
  assembles a whole frame either way, as Linux's `hid-multitouch.c` does.
- **The certification blob:** some pads want their feature report
  `0xFF00:0xC5` read before they switch modes. Linux reads it; so will
  we.

**Visible result:** in a debug build, each finger's ID, position and
confidence, printed over serial or ssh.

## 7. Step 4: the interrupt, polled first

The pad pulls a GPIO pin of the chipset low while it has a report.
- **First version: no GPIO driver.** A kernel thread reads the input
  register every 8 ms, which is the pad's own report rate (125 Hz). Most
  pads answer a report of length 0 when nothing is new. If the MSI's
  doesn't, the GPIO driver below comes first.
- **Then the GPIO interrupt.** The Tiger Lake GPIO controller (ACPI
  `INT34C6`; Linux `drivers/pinctrl/intel/pinctrl-intel.c` and
  `pinctrl-tigerlake.c`) has its registers' address and shared IRQ in its
  `_CRS`. The driver sets the pad's pin as a level-low interrupt,
  acknowledges it, and the kernel then reads the pad only when a finger
  moves.
  - That means no wake-ups at all when idle: CLAUDE.md, rule 8.
  - Before and after: wake-ups per second and CPU time with the pad idle
    and in use, in `docs/performance.md`.
  - About 300 lines.

## 8. Step 5: multitouch on /dev/input

`/dev/input/event2` uses Linux's **multitouch protocol B**
(`Documentation/input/multi-touch-protocol.rst`). It carries the pad's
own name and IDs, as Linux would show them, because libinput's quirks
match on them.

| What | Events | From |
|---|---|---|
| Fingers | `ABS_MT_SLOT`, `ABS_MT_TRACKING_ID`, `ABS_MT_POSITION_X/Y` | Contact Identifier, X, Y |
| **Palms** | `ABS_MT_TOOL_TYPE` = `MT_TOOL_PALM` | Confidence 0 (and it stays a palm until lifted) |
| Finger count | `BTN_TOUCH`, `BTN_TOOL_FINGER` … `BTN_TOOL_QUINTTAP` | Contact Count |
| The click | `BTN_LEFT` | Button 1 |
| Time | `MSC_TIMESTAMP` | Scan Time (libinput uses it for speed) |
| What it is | `INPUT_PROP_POINTER`, `INPUT_PROP_BUTTONPAD` | a clickpad |
| Old readers | `ABS_X`, `ABS_Y` | the first finger |

**The ioctls libinput and libevdev ask:**
- `EVIOCGABS`: min, max and **resolution in units per millimetre**, from
  the descriptor's physical size, since libinput thinks in millimetres;
- `EVIOCGPROP`;
- `EVIOCGBIT` for `EV_ABS` and `EV_MSC`;
- `EVIOCGMTSLOTS`, to catch up after a dropped queue;
- `EVIOCGKEY`.

**Access:** like event0 and event1, only the X server and the login
screen may read it (group `input`): a touchpad file is as private as the
keyboard's.

About 300 lines in `drivers/evdev.c`.

**Testing without the MSI:** QEMU's `virtio-multitouch-pci` device
(virtio-input, QEMU 8.1 and later) sends multitouch events. A small
virtio-input driver (about 200 lines) lets steps 5 and 6 be built and
tested in QEMU, `make test` included.

**Visible result:** `evtest` or `libinput debug-events` shows the
fingers, and palms as palms.

## 9. Step 6: libinput

- **Packages** (Alpine; `live/packages.txt`, downloaded at build time):
  - `libinput`, which pulls in `libevdev` and `mtdev`;
  - `xf86-input-libinput`;
  - `libudev-zero`. libinput asks udev what a device is, and MyOS has no
    udev; libudev-zero answers from the device file itself. It must
    report the pad as a touchpad (`ID_INPUT_TOUCHPAD`), otherwise a
    quirks entry says so.
- **X** (`live/overlay/etc/X11/xorg.conf.d/10-myos.conf`): event2 gets
  `Driver "libinput"` with:
  - `ScrollMethod "twofinger"`, `NaturalScrolling "true"`;
  - `Tapping "true"`, `ClickMethod "clickfinger"` (two fingers pressing =
    right-click);
  - `DisableWhileTyping "true"`.

  The PS/2 mouse and keyboard stay on evdev for now.
- **Quirks** (`/etc/libinput/local-overrides.quirks`): the PS/2 keyboard
  is `AttrKeyboardIntegration=internal`, so "while typing" pairs it with
  the pad, and the pad is internal too.
- **What programs see:**
  - smooth, pixel-by-pixel scrolling through XInput 2.1 scroll valuators
    (GTK3 programs, Firefox, Chromium);
  - older programs get wheel clicks;
  - pinch and swipe arrive as XInput 2.4 gesture events (X server 21.1
    and later). Few programs use them yet, and Mission-Control swipes
    would be the window manager's job, later.

**Visible result:** two-finger scrolling and palm rejection on the MSI.

## 10. Palm rejection, layer by layer

| Layer | Who | How | Needs |
|---|---|---|---|
| 1. The pad's own judgement | the pad → kernel | Confidence 0 → `MT_TOOL_PALM`; libinput ignores the contact | steps 3 and 5. **The strongest signal, and free** |
| 2. Ignore the pad while typing | libinput | touches are ignored for a short time after each key press (about 0.2–0.5 s) | the keyboard marked internal (quirks) |
| 3. Edge zones | libinput | a touch that starts in the pad's side strips and stays there is ignored | the pad's size in millimetres (`EVIOCGABS`) |
| 4. Size and thumbs | libinput | very large contacts, and a resting thumb at the bottom edge, are ignored | the size, if the pad reports it (`ABS_MT_TOUCH_MAJOR`) |

## 11. Other machines

- **GPD Pocket 4:**
  - Its pointer is a USB device that reports itself as a mouse and
    scrolls in its own firmware, so it needs USB HID (B50), with the same
    HID parser.
  - Its touchscreen is I2C HID multitouch (`NVTK0603`) on AMD's I2C,
    which is the same DesignWare core, plus an AMD GPIO driver. libinput
    turns it into touches.
- **Old laptops** (Pentium M, Pentium 4): PS/2 touchpads on the 8042's
  second port, each maker with its own protocol: Synaptics (most), ALPS,
  Elantech (Linux `drivers/input/mouse/`).
  - Synaptics "absolute mode" gives position, pressure and finger width:
    palms are told apart by width.
  - Older pads report how many fingers there are, but one position
    ("semi-multitouch"). libinput still scrolls with two fingers on
    those.
  - A per-maker driver in `drivers/mouse.c`, later.

## 12. Settings › Trackpad

A Trackpad page beside Mouse & Keyboard, as on the Mac:
- Tap to click;
- Natural scrolling;
- Ignore the trackpad while typing;
- Click with two fingers for a right-click;
- Tracking speed.

Changes apply at once through the device's libinput properties (as
`xinput set-prop` does) and are saved for the next login.

## 13. Steps

Each step ends with something to see.

1. **ACPI tables in `/sys/firmware/acpi/tables`.** Dump the MSI's over
   ssh, decompile them on the Mac (`iasl`), and write down the pad's
   address, pin, descriptor register and timings.
2. **The LPSS I2C controller.** The boot log shows the pad's HID
   descriptor (`06CB:7E7E` or `04F3:30AA`).
3. **The HID parser and precision-touchpad mode.** A debug build prints
   the fingers and their confidence.
4. **Multitouch `event2`.** `evtest` shows the slots and palms. In
   parallel: virtio-multitouch for QEMU tests.
5. **libinput in X.** Two-finger scrolling and palm rejection work.
6. **The GPIO interrupt instead of polling.** Idle wake-ups before and
   after.
7. **Settings › Trackpad.**

**Size:** about 2,000–3,000 lines of kernel C:

| Piece | Lines |
|---|---|
| ACPI tables | 150 |
| I2C | 400 |
| HID parser | 600 |
| i2c-hid | 300 |
| `evdev` multitouch | 300 |
| GPIO | 300 |
| virtio-input | 200 |
| board table | 50 |

Plus three packages and an X config change.

**Risks:**
- The pad's wiring is only in the DSDT; step 1 settles that first.
- LPSS reset and clock details come from Linux's source, not a datasheet.
- Some pads need the certification blob read before changing mode.
- libudev-zero may need a quirks entry to call the pad a touchpad.
- I2C transfers must stay interrupt-driven: no busy-waiting.
