# Power & Performance — Settings requirements

This document specifies what **Settings** must offer for choosing between **real-time
guaranteed throughput** and **power savings**, what each mode controls, and what the
**kernel**, **GPU/display stack**, and **Settings application** must implement to support
it. It extends the scheduling model in `docs/DESIGN.md` and the display plan in the same
file; it does not replace them.

**Audience:** kernel work, display drivers, and `live/myos-settings` (plus the small
user-space helpers that apply policy at login and on change).

---

## 1. Goals

1. **One place in Settings** where the user (or administrator) picks the machine’s
   performance posture and tunes knobs for that posture.
2. **Real-time mode:** time-partitioned scheduling with a **visible 20-slot major frame**,
   **system slots fixed** (a **system class**, not only the `main` thread), named tasks
   owning the rest (including **multi-slot** tasks), **IRQs that do not silently steal
   those slots**, and **no power-saving actions that would break timing guarantees**.
3. **Power-saving mode:** the opposite trade — **variable CPU frequency** (including a
   ceiling and turbo), **deep idle**, **fewer timer wakeups**, **disk and device idle**,
   **display dim / blank / backlight**, **GPU slowdown while the picture is still up**,
   and a separate **computer-sleep** timer, without pretending to offer hard real-time
   bounds.
4. **Mutually exclusive primary modes** at the policy level: when real-time guarantees are
   **on**, power-saving mechanisms that add jitter or stretch latency are **off** (or
   clamped to safe minimums documented below). When real-time is **off**, the slot table
   may still exist for diagnostics but **does not drive preemption**; best-effort scheduling
   and power policy apply instead.
5. **Persistence** survives reboot on an installed disk; **apply without reboot** where
   the kernel can; clear messaging when a change needs a restart or re-login.
6. **Shipped default is power saving** once this policy exists. Today `sched_start()`
   always runs the 20-slot table; turning that into an explicit `mode=` is a **behavior
   change** (see §3.3). Sleep has a **resume** path, not only an enter path.

---

## 2. User interface (Settings)

### 2.1 Placement and navigation

- New sidebar pane: **“Power & Performance”** (search keywords: power, battery, real-time,
  slots, CPU, screen, sleep, throughput).
- Optional sub-pages later (e.g. **Real-Time Slots** as a drill-in); v1 may be one scrollable
  pane with two sections separated by a master control.

### 2.2 Master control

A single **mode switch** (radio or segmented control), not two independent on-switches:

| Mode | Label (user-facing) | Meaning |
|------|---------------------|---------|
| **A** | **Real-time throughput** | Guaranteed 1 ms slot timing; power savings disabled per §4.2. |
| **B** | **Power savings** | Variable clocks, idle, display/GPU savings; no RT guarantees. |

Copy under the control (footnote style, like Displays):

- Real-time: *“Reserves time on the CPU for named tasks. Use for audio, control, or anything
  that must run on a fixed schedule.”*
- Power savings: *“Lets the processor and screen rest when idle. Ordinary programs may pause
  longer or run in bursts.”*

**Administrator gate:** changing mode or editing the slot map affects **all users** and the
whole machine. Require the same **administrator password** flow as **Users & Groups**
(`pam` + `wheel`), and write system config under **`/etc/myos/`**. Per-user overrides are
out of scope for v1: one machine policy, not a file under `~/.config`.

### 2.3 Real-time section (visible when mode A is selected)

#### 2.3.1 Major-frame diagram

- Show **20 slots** in order, labeled **1–20** (matches the kernel’s `sched_slot()` and
  `config/slots.conf`; slot index **0 is unused** in the kernel table).
- **Slots 1 and 10** are visually distinct and **not assignable** to user tasks:
  - Label: **“System (OS)”** — the **system class** (§4.1): the kernel `main` thread
    (`SCHED_SYSTEM_THREAD`) plus other threads Settings or the kernel marks as system
    (login, apply helper, and optionally the display server). `docs/DESIGN.md` already
    says these slots are for the kernel **and** system user-space; they are not `main`
    alone.
- Remaining slots **2–9, 11–20** (18 slots) are assignable to **named real-time tasks**.

Visual design (requirements only):

- One horizontal or wrapped row of 20 cells; each cell = **1 ms**; tooltip: *“Slot *n*,
  1 ms, frame repeats every 20 ms.”*
- Color: system slots fixed color; assigned slots show **task name** (truncated).
- **Unassigned** = no name in the config. **Idle (no process)** = a name is configured
  but no living thread is bound. Do not draw those two the same.
- Optional timeline annotation: *“20 ms major frame”* under the row.

#### 2.3.2 Task list and slot assignment

- **Task table:** each row = one **named real-time task** (string, max **15 characters**,
  same as `config/slots.conf` today).
- **Slots column:** multi-select or drag onto the diagram; a task may own **one or many**
  slots (example from design: `brakes` → 4, 9, 14, 19).
- **Constraints enforced in UI and kernel:**
  - At most **one owner per slot** (except system slots 1 and 10).
  - System slots **cannot** be reassigned.
  - Duplicate names merge into one task with the union of slots.
- **Process binding:** a documented **`prctl`/syscall** registers *“this process is task
  `radio`”*. The kernel **does not** grant slots because a thread happened to be named
  `brakes`. Only **uid 0**, or a process whose name appears in the admin-written table
  and that holds the bind token the helper issued, may attach. A second process that
  claims an already-bound name is **rejected**.
- Buttons: **Add task**, **Remove task**, **Reset to defaults** (reload shipped
  `config/slots.conf` example).

#### 2.3.3 Real-time knobs (mode A)

These apply only when mode A is active:

| Knob | Type | Default | Requirement |
|------|------|---------|-------------|
| **Major frame length** | read-only v1 | 20 ms | Display **20 × 1 ms**; future: configurable slot count/width in kernel. |
| **Slot overrun policy** | choice | Preempt at slot end | Hard preempt when the tick advances; document in UI. |
| **Best-effort during RT** | toggle | On | When on, non-slot threads run in **leftover** slot time (current `sched.c`
  behavior). When off, only slot owners + idle + the **system class** run. Settings
  **refuses Off** unless at least the apply helper and a login/shell path stay in the
  system class — otherwise the machine cannot be administered. |
| **IRQ policy** | read-only v1 | Defer to system slots | Footnote: *“Disk and network interrupts are finished in slots 1 and 10 when they
  can be; time they steal from a named slot counts as that task’s overrun.”* |
| **Show live slot indicator** | toggle | Off | Optional: poll `/proc` or syscall for **current slot** and highlight cell (nice-to-have v1). |

No sliders for CPU frequency, dim, display-off, or sleep in mode A — those controls are
**hidden or disabled** with explanation: *“Unavailable in real-time mode.”* Manual
**Turn display off now** may still appear; it blanks the framebuffer only (§4.2).

### 2.4 Power-saving section (visible when mode B is selected)

Show a short **status line** while this pane is open: current CPU frequency if the kernel
reports it, current idle depth (C1 / deeper / unknown), and whether the display is dimmed,
blanked, or awake. Any knob the hardware cannot do is **visible but disabled**, with the
reason (*“Fixed frequency — this CPU has no speed control”*, *“No backlight driver”*).
Do not hide the row.

**On AC vs on battery.** Two copies of the power-saving knobs below, selected by a
segmented control **On battery** / **Plugged in** (default view follows the current
supply). A desktop with no battery shows only **Plugged in**. Lid-close and sleep still
apply on both.

#### 2.4.1 CPU and idle

| Knob | Type | Default (battery / AC) | Maps to |
|------|------|------------------------|---------|
| **CPU power policy** | enum | Power saver / Balanced | **Performance** / **Balanced** / **Power saver**. Sets the **maximum** slider and turbo below. There is **no minimum-frequency slider** in v1 (the CPU may drop on its own when idle). |
| **Maximum CPU speed** | slider | 100% / 100% of the highest P-state the policy allows | Caps frequency. Disabled when the CPU cannot change speed. |
| **Allow turbo / boost** | toggle | Off / On | Highest turbo bin. Off in Power saver. Firmware thermal limits may still cut turbo (§4.2); the status line says *“Limited by temperature”* when that happens. |
| **Deepest idle** | enum | Deepest available / C1 only | **C1 only** (`hlt`), or **C2**, **C3**, … up to what ACPI **`_CST`** lists. “Deepest available” is the default on battery. |
| **Timer coalescing** | toggle | On / Off | APIC **one-shot** next event vs a periodic 1 kHz tick (§4). |

Changing **CPU power policy** moves the max-speed slider and turbo toggle to that
policy’s preset; the user may then override those two without leaving the policy name
(stored as `cpu_max_pct` and `turbo`, not a hidden min).

#### 2.4.2 Display, GPU, and sleep

Blanking the panel and sleeping the computer are **separate timers**.

| Knob | Type | Default (battery / AC) | Maps to |
|------|------|------------------------|---------|
| **Brightness** | slider | 40% / 80% | `/sys/class/backlight/*/brightness` when a backlight exists. Disabled with an explanation until then (Pocket 4: `amdgpu_bl1`). |
| **Dim display after** | slider + presets | 2 min / 10 min | **Never**, or minutes. Target brightness is **Dim level**. |
| **Dim level** | slider | 10% | Used only when dim is not Never. |
| **Turn display off after** | slider + presets | 5 min / 30 min | **Never**, **1 min**, **5 min**, **10 min**, **30 min**, **1 hr**, or custom minutes. Must be **≥ dim** when both are set. Sequence in §5. |
| **Slow graphics while the screen is on** | toggle | On / Off | Lower refresh if the driver can, damage-only framebuffer copies, drop GPU clocks while the desktop is still visible (§5.3). Independent of the off timer. |
| **Turn compositor off in this mode** | toggle | On / Off | When on, the session **does not start** `picom` (does not delete the package). Mid-session change: **send IceWM the existing restart** (or kill `picom` and do not respawn) before the toast says Applied. Next login also honors the file. Footnote shows the installer’s `desktop.conf` choice. |
| **Require password when the display wakes** | toggle | On / On | Blank is not a lock by itself. Wake runs the existing login/PAM path (or a lighter lock screen that uses the same password). |
| **Sleep computer after** | slider + presets | 15 min / Never | **Never**, or minutes **after the display is already off** (not from last keypress if the display timer is shorter). **Sleep** means firmware **`s2idle`** on the Pocket 4 (`docs/hardwareRequirements.md`); S3 only where the board supports it. Distinct from **power off** (`_S5`). Resume is §4.2. |
| **When the lid closes** | enum | Sleep / Do nothing | **Sleep**, **Turn display off**, **Do nothing**. No lid: hide the row. |
| **When the power button is pressed** | enum | Sleep / Sleep | **Sleep**, **Power off**, **Ignore**. Short press only; hold still forces power off at the EC if the firmware does that. Hidden if there is no distinct power button event. |
| **Low battery** | enum + % | Sleep at 5% / Off | **Off**, or **Sleep** / **Power off** when charge is at or below N% (default 5). A warning at 10% (or 2× N) is required when a battery exists. Hibernate is out of scope. |

**Manual actions** (buttons, not timers): **Turn display off now**, **Sleep now**.

**Brightness keys.** Hardware / Fn brightness writes the same backlight sysfs node
Settings uses. `myos-powerd` watches that node and refreshes the slider; it does not
fight a keypress. Dim and display-off still use the last user-set brightness as the
restore value.

#### 2.4.3 Disk, USB, and wireless

These stay available in mode B only. Each row disables itself when the driver is missing.

| Knob | Type | Default (battery / AC) | Maps to |
|------|------|------------------------|---------|
| **Spin down hard disks** | toggle + minutes | On, 10 min / Off | Rotational disks only. Default idle **10 minutes** when on. SSDs and NVMe ignore spin-down; they use link power below. |
| **Storage link power** | toggle | On / Off | AHCI link power management and NVMe APST when those drivers exist. |
| **USB autosuspend** | toggle | On / Off | Autosuspend idle USB devices (webcam, unused hubs). Human interface (keyboard, touchpad, touchscreen) stays awake so wake-from-blank still works. |
| **Wi-Fi power save** | toggle | On / Off | Driver power-save (Pocket 4: `iwlwifi`) when associated and idle. |
| **Unused wired Ethernet** | toggle | On / Off | Allow the NIC to idle when no cable / no addresses. Optional; disable the row if there is no Ethernet driver. Bluetooth follows the same pattern if a radio appears later. |

#### 2.4.4 What counts as “in use”

The display and sleep timers reset on:

- keyboard, pointer, and touchscreen events;
- lid open;
- an explicit **inhibit** (below).

They do **not** reset on network traffic alone.

**Inhibit (v1: one bit).** A program that is showing video or a slideshow (Preview, a
future player) takes an inhibit so **dim, display-off, and sleep** all pause. Interface:
`/run/myos/inhibit-display`, held by an open fd or a pid file with a live pid. A dead
pid does not inhibit. Settings does not list inhibitors in v1; the status line may say
*“Display sleep paused”*.

v1 does **not** split “keep the picture” from “allow sleep” (Linux `idle` vs `sleep`
inhibit). A later revision may add a second file; until then Preview must take the one
bit if either outcome would be wrong.

#### 2.4.5 Behavior when mode B is on

- **Slot diagram** is greyed and read-only (*“Real-time scheduling inactive”*) or hidden.
  No assignment edits.
- **Best-effort** scheduling is primary; the status area does not claim timing guarantees.
- If boot rejected a real-time config and fell back to this mode (§4.3), show a banner:
  *“Real-time settings were invalid and were not applied.”*

---

## 3. Configuration files and apply path

### 3.1 System policy file

**Path:** `/etc/myos/power-performance.conf`

Suggested keys (INI-style, like other MyOS config):

```ini
# mode: realtime | powersave
mode=powersave

[realtime]
best_effort_in_leftover=1
# tasks: name=slot,slot,... (system slots 1,10 omitted)
brakes=4,9,14,19
radio=6,16

# Two profiles. Keys match §2.4. 0 minutes = never.
[powersave.battery]
cpu_policy=powersave
cpu_max_pct=60
turbo=0
idle_max=deepest            # c1 | c2 | c3 | deepest
timer_coalesce=1
brightness_pct=40
dim_after_minutes=2
dim_level_pct=10
display_off_minutes=5
gpu_slow_while_on=1
compositor_off=1
lock_on_wake=1
sleep_after_display_off_minutes=15
lid_close=sleep             # sleep | display_off | nothing
disk_spindown=1
disk_spindown_minutes=10
storage_link_power=1
usb_autosuspend=1
wifi_powersave=1
ethernet_idle=1
power_button=sleep          # sleep | poweroff | ignore
low_battery_action=sleep    # off | sleep | poweroff
low_battery_pct=5

[powersave.ac]
cpu_policy=balanced
cpu_max_pct=100
turbo=1
idle_max=c1
timer_coalesce=0
brightness_pct=80
dim_after_minutes=10
dim_level_pct=10
display_off_minutes=30
gpu_slow_while_on=0
compositor_off=0
lock_on_wake=1
sleep_after_display_off_minutes=0
lid_close=nothing
disk_spindown=0
disk_spindown_minutes=10
storage_link_power=0
usb_autosuspend=0
wifi_powersave=0
ethernet_idle=0
power_button=sleep
low_battery_action=off
low_battery_pct=5
```

- **Build-time** `config/slots.conf` remains the compiled fallback when no runtime file
  is readable (§3.3). The helper, not Settings, writes the runtime file (§3.2).

### 3.2 Who applies what

| Writer | What it may change | How |
|--------|--------------------|-----|
| **Settings** (as the user) | Nothing on disk or in the kernel | Draws the pane; on Save talks to the helper. |
| **`myos-apply-power-performance`** (setuid / `CAP_SYS_ADMIN`) | `mode`, `[realtime]` slot map, bind tokens, `cpu_*` / `idle_max` / `turbo` / `timer_coalesce` | Writes the file (temp + rename), then **ioctl** / syscall so the kernel swaps atomically. |
| **`myos-powerd`** (runit, root) | Dim, blank, backlight, sleep, lid, power button, low-battery action, inhibit watch | **Reads** the file; does **not** rewrite `mode` or the slot map. |

Do not give both programs write access to the same keys.

**`myos-powerd`** starts at boot (login screen included), not only from `xsession`. It
selects `[powersave.battery]` or `[powersave.ac]` from the kernel’s AC bit, arms the
timers in §2.4, and does **none** of that list while `mode=realtime` except **Turn
display off now**.

### 3.3 Boot, live disk, and the shipped default

- **`sched_start()` today always turns slots on.** After this policy exists, the **shipped
  default is `mode=powersave`**. Real-time is opt-in from Settings. That is an intentional
  change from the current kernel.
- The kernel must know `mode` and the slot map **before** a disk `/etc` is mounted:
  1. If the initramfs (or the already-mounted root) has `/etc/myos/power-performance.conf`,
     parse it;
  2. else use the **compiled** `slot_config[]` **and** `mode=powersave` (slots loaded but
     **not driving** the scheduler);
  3. after the real root is mounted, re-read the file once and apply (no second
     `sched_start()`).
- Do **not** delay `sched_start()` until userspace is up; boot stays single-threaded
  round-robin until that call, as now.
- **Live USB:** `/etc/myos` often lives in RAM and is lost at power-off (`docs/gaps.md`).
  Settings must say *“These settings last until you shut down (live disk).”* An installed
  `myos-root` persists the file.

### 3.4 Switching modes while the machine is up

- **A → B:** finish the current 1 ms slot, then stop rotating the table; drop bind
  tokens’ effect on pick (names may stay in the file); enable power-saving knobs.
  Running RT tasks become best-effort. No reboot.
- **B → A:** validate the slot map first; refuse if `best_effort_in_leftover=0` and the
  system class is empty (§2.3.3). Then enable the table on the next tick. Tasks that
  already hold a valid bind run in their slots; others stay best-effort until they bind.
- **`compositor_off`:** apply as in §2.4.2 (restart or kill `picom`) before reporting
  success. Do not wait for the next login unless IceWM cannot be told.

---

## 4. Kernel requirements

### 4.1 Scheduling and real-time mode (mode A)

**Existing behavior to preserve and expose**

- **20 slots × 1 ms**, slots **1 and 10** reserved for the **system class** (today only
  `main` is bound; that is not enough — see below).
- Slot owners run for the **full slot**; timer tick **forces** slot advance and preemption
  at the boundary (overrun policy).
- **Best-effort** threads in leftover time when enabled (`sched_pick` / `next_best_effort`).

**New requirements**

1. **Global policy flag:** `sched_policy_realtime` (or equivalent):
   - **On:** `sched_start()`-style slot table **active** (current `slots_on`).
   - **Off:** slot table **inactive**; round-robin + preempt; the diagram still reads the
     file. Do **not** increment `sched_slot()` for pick; a debug counter is optional.

2. **Runtime slot table:** load from `/etc/myos/power-performance.conf` as in §3.3.
   **`rebuild_slot_table()`** on an atomic apply.

3. **System class (slots 1 and 10):** more than `SCHED_SYSTEM_THREAD`. The class always
   includes `main` and `idle`. The apply helper, `myos-powerd`, and the login path must
   be markable as system so `best_effort_in_leftover=0` cannot lock the administrator
   out. Optional later: the X server. `sched_pick` in those two slots runs any ready
   system-class thread (round-robin among them), not only `main`.

4. **Named-but-dead slots:** if the config names `radio` and no thread is bound, the
   slot is **Idle (no process)**. If `best_effort_in_leftover=1`, leftover rules apply.
   If it is 0, only idle runs there. Report the two states separately to `/proc`.

5. **Bind is privileged:**
   - `prctl` / syscall attaches the calling process (all its threads, v1) to a name in
     the admin table.
   - The kernel checks: name exists, slot set non-empty, **uid 0 or a one-time token**
     issued by the apply helper for that name, and the name is not already bound.
   - Matching `thread_create("brakes")` by string **is not enough**.

6. **IRQs and the 1 ms frame:**
   - Device interrupts may still preempt a slot (x86 cannot wait).
   - **Bottom-half work** (the rest of the disk/NIC handler after the minimum ACK) is
     deferred to the **next system slot** (1 or 10) when mode A is on.
   - Time spent in an IRQ (top half) **during a named slot** is added to that task’s
     **overrun counter**. The guarantee is “we count and we defer what we can,” not
     “IRQs never happen.”
   - Document this on the Settings footnote (§2.3.3). Without it the diagram is false
     on a busy bus.

7. **Syscall or `/proc` interface:**
   - Read: mode, slot owners, bound pids, current slot, Idle vs Unassigned, jitter,
     overruns (including IRQ overruns), capability bits (speed, turbo, `_CST`, backlight,
     lid, AC adapter, sleep, disk spin-down, USB, Wi-Fi, Ethernet).
   - Write: admin-only; validate; apply atomically (§3.4).

8. **Power-saving inhibition when mode A is on:** C1 only, fixed high P-state, no tick
   coalescing of `sched_tick()`, and the rest of the §4.2 inhibit list.

9. **Measurement:** slot jitter and overrun counts per task (`docs/performance.md`).
   Firmware thermal throttle in mode A is reported; it **breaks** the 1 ms guarantee and
   must not be hidden.

### 4.2 Power-saving mode (mode B)

**CPU frequency**

- **Policy plus explicit ceiling:** `cpu_policy` picks a preset; `cpu_max_pct` and `turbo`
  are the knobs Settings stores after the user moves them (§2.4.1).
- **i686 Pentium M:** ACPI **`_PSS`** or SpeedStep MSRs where CPUID allows. **Pentium 4**
  often cannot change speed: report `fixed` on the status interface and leave the sliders
  disabled. Do not pretend a write succeeded.
- **x86_64 (Pocket 4):** ACPI **CPPC** or the AMD p-state interface once ACPI tables are
  parsed; until then the same `fixed` report.
- **Idle path:** keep **`cpu_idle()` → `sti; hlt`** (C1). When `idle_max` is deeper than
  C1, enter ACPI **`_CST`** or **`mwait`** only in mode B, and never deeper than the
  configured ceiling. On more than one logical CPU, a package idle state is entered only
  when **every** CPU is idle at that depth (future SMP); until SMP, one core is the whole
  package.
- **Status for Settings:** current frequency (kHz or “fixed”), idle depth last used,
  thermal-limited bit, and capability flags: speed control, turbo, `_CST` levels,
  backlight, lid, AC adapter, sleep, disk spin-down, USB autosuspend, Wi-Fi power save,
  Ethernet idle.

**Timer and wakeups**

- Implement **local APIC one-shot** scheduling for non-RT timers (`docs/performance.md`):
  next wake = min(next POSIX timer, next TCP timer, next display-policy deadline if the
  kernel owns it, …), **not** a fixed 1 kHz tick when nothing needs it.
- When mode B is on, **do not** rotate the slot table. Best-effort preemption may use a
  coarser tick or one-shot deadlines.
- Screen-off must not by itself keep a 1 kHz tick or a full-frame blit running.

**Disk and devices**

- **ATA:** when no thread is runnable, do not busy-poll the status port (`docs/gaps.md`,
  `POLL_FIRST` in `drivers/ata.c`). Sleep on the disk interrupt. This applies in mode B
  always; in mode A keep the poll only inside a slot that is allowed to wait on I/O, and
  document the latency.
- **Spin-down and link power:** honor `disk_spindown`, `disk_spindown_minutes`, and
  `storage_link_power` when the storage driver exists (IDE/AHCI/NVMe). No-op with a
  capability flag until then.
- **USB autosuspend**, **Wi-Fi power save**, and **unused Ethernet idle:** same pattern.
  Never autosuspend the keyboard, mouse, or touchscreen nodes that `myos-powerd` uses
  as wake sources.
- **AC vs battery:** a stable “on battery” bit (ACPI AC adapter or equivalent).
- **Charge percent** when a battery exists, for the low-battery action.

**Sleep and resume**

- **Display blank is not sleep.** Sleep is a separate request: firmware **`s2idle`** on
  the Pocket 4; **S3** only when the board’s ACPI says so. **Power off** remains **`_S5`**
  (`docs/gaps.md`, `drivers/power.c`) and is not this slider.
- Until those tables are implemented, **Sleep now** and the sleep timer return **“not
  supported”** instead of `hlt`.
- **Before sleep:** sync dirty file data (`sync`); do not remount read-only in v1;
  stop the GOP blit thread; blank and backlight 0 if not already.
- **During sleep:** `CLOCK_MONOTONIC` does **not** jump (it excludes suspend, as on
  Linux). Wall-clock/`CLOCK_REALTIME` may jump; NTP or `hwclock` after resume is
  userspace’s job. Document this for Preview and timers.
- **Resume:** restore devices the kernel already owns (PIT/APIC, input, storage,
  `/dev/fb0`). If GOP/`simplefb` is gone after `s2idle`, fail visibly (serial + a
  console line) rather than writing a stale BAR. X may need a restart — the toast
  after **Sleep now** must say so if the framebuffer cannot be reattached.
- Wake sources (minimum): power button, lid open, keyboard. Wake-on-LAN stays **off**
  unless a later knob says otherwise.

**Power button, lid, low battery, thermal**

- Power-button short press follows `power_button` (sleep / poweroff / ignore).
- Lid follows `lid_close`.
- When charge ≤ `low_battery_pct` and `low_battery_action` is not `off`, do that
  action even if the sleep timer is Never. Warn at 10% or 2× the threshold, once
  per discharge.
- **Fan and thermal** on the Pocket 4 stay with the EC (`docs/hardwareRequirements.md`).
  The kernel reports *thermal limited* when frequency drops for heat. It does not
  ship a second fan curve in v1. In mode A, that report is how we admit the 1 ms
  guarantee was broken by firmware.

**Display policy hooks (kernel side)**

- **`FBIOBLANK`** on `/dev/fb0` (already wired for **Bochs DRM** via `drm_blank()`).
- **`simplefb` (GOP):** **`fb_blank`**, and **damage-aware blit** whenever X has the
  shadow mapped — including when the off-timer is **Never** and `gpu_slow_while_on=1`.
  A blanked screen stops the blit thread entirely (`drivers/video/simplefb.c`).
- **Backlight:** `/sys/class/backlight/*` when **amdgpu** (or another driver) registers
  one. Until then the capability flag is clear.

**Inhibit list when mode A**

- P-state changes, turbo policy, C-states below C1, timer coalescing that delays the
  1 ms slot tick, disk spin-down, USB autosuspend, Wi-Fi power save, GPU clock drops,
  refresh changes, display-off, dim, and system sleep are all **off** while
  `mode=realtime`. Manual **Turn display off now** may still blank the framebuffer; it
  must not enter s2idle.

### 4.3 Permissions and safety

- Only **uid 0** or a dedicated **`/dev/myos/sched`** device with admin-only write may
  change mode or slot map.
- Invalid configs **reject** with errno and leave prior policy active.
- **Fail safe:** if the real-time config is invalid at boot, fall back to **powersave**
  (battery or AC profile), log to serial, and set a flag Settings shows as the banner in
  §2.4.5. Do not silently keep a half-applied slot map. A missing file is not an error:
  shipped default is **powersave** (§3.3).

---

## 5. GPU and display requirements

These serve **mode B** primarily. In **mode A**, idle dim / display-off / sleep **do not
run**. The **Turn display off now** button (§2.4.2) may still blank the framebuffer; it
must not sleep.

### 5.1 Blank and unblank

| Driver path | Requirement |
|-------------|-------------|
| **Bochs / `drm_fbdev`** | Already: **`fb_blank` → `drm_blank` → pipe disable**. Session uses
  **`FBIOBLANK`** (`FB_BLANK_POWERDOWN` / `FB_BLANK_UNBLANK`). |
| **`simplefb` (UEFI GOP)** | Add **`fb_blank`**: stop **`shadow_sync`** thread; optionally fill black
  on GOP mapping; document that firmware may still scan out. |
| **Future `amdgpu`** | KMS **`disable`/`enable`** on CRTC; panel **DPMS**-like behavior via AMD
  display core; follow Linux **`drm_mode_setcrtc`** power sequencing. |

### 5.2 Backlight and dim

- When a backlight device exists (**`amdgpu_bl1`** on the Pocket 4,
  `docs/hardwareRequirements.md`), **Brightness**, **Dim level**, and off-to-zero use
  `/sys/class/backlight/*/brightness` scaled 0–max.
- **While the screen stays on:** brightness is the user’s slider, unless the dim timer
  has fired, in which case it is **Dim level** until input or inhibit.
- **On display-off timeout:** dim (if not already) → **`FBIOBLANK`** → backlight **0**.
  On input: restore the pre-dim brightness → unblank → if `lock_on_wake`, present the
  password UI before the session is usable.
- **On sleep timeout** (only after the display is off, and only if sleep is supported):
  enter s2idle/S3. Waking returns through the same lock if `lock_on_wake` is set.
- No backlight driver: skip brightness writes, still blank via **`FBIOBLANK`** when that
  ioctl works, and disable the brightness and dim-level sliders.

### 5.3 GPU while the picture is still visible (mode B)

`gpu_slow_while_on` is **not** “after blank.” It applies whenever the desktop is up:

- **i386 GOP shadow:** copy to the real framebuffer only for damaged lines, not a full
  `memcpy` every 16 ms (`fbblit` in `drivers/video/simplefb.c`).
- **Refresh:** if the driver can set a lower refresh for the same resolution, it may do
  so while this knob is on. If it cannot, say so in the capability flags; do not change
  resolution out from under X.
- **GPU clocks:** when `amdgpu` exists, apply an idle power profile while this knob is
  on and the screen is not blank. v1 without `amdgpu`: no clock ioctl; the blit and
  compositor rules still apply.
- **After blank:** stop the blit thread, disable the CRTC/pipe where the driver supports
  it (`drm_blank` on Bochs; `fb_blank` on simplefb; KMS disable on future `amdgpu`).
  Runtime-suspend of the GPU is allowed only while blanked and is optional until the
  driver exists.
- **Compositor:** when `compositor_off=1`, the session does not start `picom` even if
  `/etc/myos/desktop.conf` allows it. Real-time mode does not force this; it only
  footnotes the cost (§5.4).

### 5.4 Real-time interaction with display

- In **mode A**, GPU frequency scaling, refresh changes, dim, display-off timers, and
  system sleep **do not run**.
- Full-screen compositing stays available in real-time mode; the UI footnote says it
  costs extra copies. `compositor_off` is a power-saving knob only.

---

## 6. Settings application requirements (`live/myos-settings`)

### 6.1 UI implementation

- New **`page_power_performance()`** registered in the panes table (§2.1).
- Reuse existing patterns:
  - **`group_new` / `row_value`** for knobs;
  - **admin dialog** from Users & Groups for Save;
  - **scale/icon** styling consistent with Displays.
- **Slot diagram:** custom **`GtkDrawingArea`** or **`GtkGrid`** of 20 widgets; drag-drop
  optional v2 — v1 can use **multi-select list** of slot numbers per task.

### 6.2 Backend

- **Read** `/etc/myos/power-performance.conf` (and `/proc` status). Writes go through
  the helper only.
- On Save (after admin OK):
  1. Invoke **`myos-apply-power-performance`**, which writes the file and the ioctl
     (§3.2). Settings does not write `/etc/myos` itself.
  2. Show toast: *“Applied”*, *“These settings last until you shut down (live disk).”*,
     or *“Restart the session for the compositor change.”*

### 6.3 Live status

- **Mode A:** optional 500 ms poll to highlight the current slot (`/proc/myos/sched` or
  syscall).
- **Mode B:** the same interface supplies frequency, idle depth, thermal-limited,
  AC/battery, charge percent, display state (on / dim / blank), “sleep unsupported,”
  and the capability bits in §4.1. QEMU reports almost everything unsupported except
  blank; the pane must still render.

### 6.4 `myos-powerd` (mode B)

- **Runit service**, running for the login screen and for a logged-in session (§3.2).
- Idleness from **evdev** (so the login screen counts) and, when X is up, from X input.
  Keyboard, pointer, and touchscreen reset dim, blank, and the “display is off” clock
  that gates sleep. Lid open resets them.
- **Inhibit** file/socket in §2.4.4 pauses dim, blank, and sleep. A dead pid does not
  inhibit.
- **`lock_on_wake`:** after unblank, the session or login UI demands the user password
  before restoring the desktop. The login screen’s own password field satisfies this
  when nobody is logged in.
- Settings never writes the file; the helper does. The daemon **only reads** and
  reloads on change (§3.2).
- Watches backlight sysfs so Fn brightness keys stay in sync with the slider.
- **`xset`** may be used once X is up if the fbdev driver forwards DPMS to **`FBIOBLANK`**;
  the daemon must still work with the ioctl alone when `xset` is absent or X is not
  running (console on `/dev/fb0`).

### 6.5 Dependencies

- No new GTK packages; use existing **GTK3**, **PAM** for admin.
- **`xset`** is already on the live image (`live/packages*.lock`). `myos-powerd` may call
  it; Settings does not link X11 just for DPMS.

---

## 7. Boot and test acceptance

### 7.1 Real-time mode

- With **`brakes`** on slots 4,9,14,19, a test thread logs slot numbers → only those slots
  (existing self-test pattern in `docs/LOG.md`).
- With mode A, **`make bench`** **slot jitter worst** must stay within documented bound (e.g.
  ≤ 10 µs on hardware; QEMU icount is not authoritative).
- Changing the slot map via Settings → a **bound** task moves to the new slots without
  reboot (§3.4).
- An unprivileged process that `prctl`s a name it does not own is **rejected**.
- `best_effort_in_leftover=0` is **refused** if the system class has no login/apply path.
- With a disk IRQ during a named slot, the task’s **IRQ overrun** counter increases;
  bottom-half work does not run in that slot.
- Mode A does **not** honor a display-off timeout or a P-state drop (capability /
  inhibit list). **Turn display off now** still blanks.

### 7.2 Power-saving mode

- With mode B, **`cpu_idle_cycles`** at idle is higher than the same idle in mode A
  (real Pentium M, step 12). QEMU instruction counts are not this test.
- **Display off:** after the configured idle with no input, **`FBIOBLANK`** succeeds on
  Bochs; input restores the picture within 1 s. With `lock_on_wake`, the desktop is not
  usable until the password is accepted.
- **Dim** happens at its own timeout and does not blank. **Sleep** is not attempted
  until the display is off. Where sleep is unimplemented, the status line says so and
  the machine stays up.
- **Inhibit:** a live inhibitor prevents blank for the whole idle period; removing it
  starts the timers from zero.
- **Login screen:** with no user logged in, the same display-off timer still fires.
- **GOP blit:** with `gpu_slow_while_on=1`, an idle X server does not copy the full
  framebuffer every frame (measure copies or CPU time in `fbblit`).
- **Disk:** an idle blocked read does not spin on the ATA status port for the old
  poll budget (`docs/gaps.md`).
- **Frequency:** on a CPU that reports speed control, `cpu_max_pct` changes the
  observed frequency. On a CPU that reports `fixed`, the slider stays disabled.
  Record watts on hardware in `docs/performance.md` when a meter exists.
- **Profiles:** unplugging AC (when the bit exists) swaps to the battery block without
  a reboot.
- **Low battery:** at the configured percent, sleep or power-off runs even if the sleep
  timer is Never (hardware with a battery gauge only).
- **Resume:** after a supported sleep, input and `/dev/fb0` work again, or the failure
  is printed. `CLOCK_MONOTONIC` has not jumped by the sleep duration.
- First boot with no file: **`mode=powersave`**, not an implicit slot schedule.

---

## 8. Phased delivery (suggested)

| Phase | Kernel | GPU/display | Settings + session |
|-------|--------|-------------|-------------------|
| **P0** | Policy flag default **powersave**; compiled fallback; capability bits | Document FBIOBLANK path | Pane; mode switch; AC/battery pages; disabled rows; live-disk note |
| **P1** | System class; privileged bind; Idle vs Unassigned; mode switch (§3.4); ATA sleep-on-IRQ | `simplefb` blank; damage-aware blit | Slot diagram; task editor; apply helper (only writer); fallback banner |
| **P2** | IRQ bottom-half deferral + overrun counts; APIC one-shot; idle ceiling | `myos-powerd` dim/blank/lock; inhibit fd; brightness-key watch | Display sliders; login-screen coverage; compositor apply mid-session |
| **P3** | cpufreq/CPPC; AC + charge; bind tokens | `amdgpu` blank, clocks, backlight | Live MHz / idle / thermal; turbo and max-speed sliders |
| **P4** | s2idle **and resume**; USB / Wi-Fi / Ethernet idle; storage link + spin-down minutes | Compositor forced off | Sleep, lid, power button, low battery |

---

## 9. Open questions (resolve during implementation)

1. **Hybrid profile?** User asked for exclusive modes; allow a future **“Custom”** that enables
   some knobs from each side with explicit warnings?
2. **Slot numbering in UI:** always **1–20** to match kernel (recommended); avoid 0–19 unless
   the whole stack shifts.
3. **Thread vs process binding:** v1 binds the **whole process** (every thread) to one
   name. Per-thread bind is later if a program needs it.
4. **Multi-core:** slot guarantees are **system-wide on CPU0** until SMP exists. When
   SMP arrives, decide per-CPU tables vs one table (do not block P2).
5. **System class membership:** must `main`, idle, apply helper, and login be hard-coded,
   or does Settings add X later?
6. **Password UI on wake:** reuse the login screen’s PAM conversation, or a separate
   locker? Either satisfies §2.4.2 if it checks the same password.
7. **X after s2idle:** if GOP cannot be reattached, is “restart the session” acceptable
   for v1 resume?
8. **Split inhibit** (keep picture vs allow sleep) is deferred; revisit when a player
   needs one without the other.

---

## 10. References

- `docs/DESIGN.md` — time-slot scheduling, compositor levels, display DRM shape.
- `kernel/sched.c`, `include/sched.h`, `config/slots.conf` — current slot implementation.
- `docs/performance.md` — APIC one-shot, idle measurement, slot jitter benchmarks.
- `drivers/gpu/drm/drm_core.c` — `drm_blank()`.
- `drivers/video/simplefb.c` — GOP path gaps (blank, blit thread).
- `docs/hardwareRequirements.md` — Pocket 4 backlight and **`amdgpu`** expectations.
- `live/myos-settings/myos-settings.c` — pane layout and admin patterns.
