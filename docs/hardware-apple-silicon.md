# Apple Silicon bare metal (roadmap step 15)

Target: **MyOS as the only OS** on Apple Silicon Macs (M1 MacBook Air first), not a macOS
app and not a VM. This document is **15a**: machines, constraints, and boot path. CPU-specific
code stays in `arch/aarch64/apple/`; the portable kernel stays in `kernel/`, `mm/`, `fs/`.

QEMU `virt` (step 14) remains the daily test harness. Bare metal reuses the same **Linux aarch64
boot convention**: MMU off, **EL1**, **x0 = physical DTB pointer**.

## First machine

| Model | SoC (device tree) | Notes |
|-------|-------------------|--------|
| MacBook Air (M1, 2020) | `apple,t8103` (J313) | First bring-up target in ROADMAP |
| MacBook Pro 13" (M1) | `apple,t8103` | Same SoC generation |
| Mac mini (M1) | `apple,t8103` | Easier cooling; good lab bench |

Later: M2/M3 families (`apple,t8112`, `apple,t8122`, …) once one M1 path works.

## What Apple firmware allows (constraints)

- **No PC BIOS / no “dd a FAT ISO and Option-boot”.** Apple boots through **iBoot → macOS
  boot objects** or a **chain you install** (Asahi Linux model).
- **Secure Boot / policy:** unsigned kernels are not loaded like on a PC. Practical paths:
  - **Asahi Linux installer** + **m1n1** stage (study [Asahi Linux](https://asahilinux.org/),
    [m1n1](https://github.com/AsahiLinux/m1n1)) to hand off to MyOS with a DTB and reserved
    memory map, or
  - **Reduced Security** / developer policy where applicable (document exact steps when 15b is
    tested on hardware — model and macOS version matter).
- **Single-boot MyOS-only is the first goal.** Dual-boot beside macOS is out of scope until
  internal NVMe and partition tools exist (15c).
- **Recovery / DFU:** keep a macOS Recovery or second machine available until MyOS can reliably
  chain-load. Do not repartition the internal SSD for experiments until an installer exists.

## m1n1 payload (build on the Mac)

```bash
make ARCH=arm64 m1n1-payload
```

Writes `build/arm64/m1n1/`:

| File | Purpose |
|------|---------|
| `kernel.bin` | Raw image for loaders that expect a binary @ **0x40100000** |
| `kernel.elf` | Same kernel with symbols (some chains load ELF) |
| `handoff.txt` | Load address, **x0 = DTB**, EL1 / MMU-off contract |

Serial log includes `Boot: x0=… dtb=… (x0|embedded)` so hardware bring-up can
confirm the loader passed the DTB pointer.

Step-by-step chain-load on an M1: **`docs/m1n1-chainload.md`**.

## Boot path (planned)

```
USB installer (future) or Asahi-style bootstrap
    → m1n1 (or equivalent first-stage on Apple hardware)
    → MyOS kernel @ 0x40100000  (x0 = DTB, Linux convention)
    → early UART or framebuffer text
    → same aarch64 userland story as step 14 (Alpine/musl), then full live (15e)
```

**QEMU note:** `qemu -kernel kernel.elf` often leaves **x0 = 0** for a plain ELF (no Linux
Image header). CI uses `kernel-apple-harness.elf`, which embeds the harness DTB when x0 is
zero. **m1n1 on real hardware must pass the DTB pointer in x0** (same as Linux).

**Not** the same image as:

- `make ARCH=arm64 run` (QEMU **virt** DTB, PL011 @ `0x09000000`)
- x86_64 `make live-uefi` USB (UEFI PCs only)

## Memory and device tree

- RAM **base and size come from the DTB** `/memory` reg cells (Apple uses 64-bit reg pairs).
  The bring-up kernel already parses `/memory` via `fdt_first_ram()`; Apple DTBs must pass that
  path in a host test before hardware.
- **Platform detection:** root `compatible` contains `apple,…` (see `arch/aarch64/apple/machine.c`).
  QEMU virt uses `linux,dummy-virt`.
- **UART for early printk:** Apple SoCs use **different** MMIO addresses than QEMU PL011; step
  **15b** adds an Apple early console driver under `arch/aarch64/apple/` (DT `stdout-path` or
  known debug UART — follow Asahi/m1n1 notes when wiring hardware).

## Storage (15c preview)

- Internal disk: Apple NVMe (IOP/NVMe stack in Linux/Asahi). MyOS will need either a shimmed
  driver path or the same bring-up order Asahi uses — not the PC AHCI/NVMe drivers in this tree
  today.
- **USB stick:** installer medium until internal `myos-root` works; not “raw `kernel.elf` only”.

## Input, display, live userland (15d–15e preview)

- Built-in keyboard/trackpad: Apple HID / I2C/SPI path from DT — after serial boot works.
- Panel: simple framebuffer or KMS-style path enough for `myos-login` / agetty.
- **`live/packages.aarch64.txt`:** Alpine aarch64 world matching x86_64 live where packages exist;
  `make ARCH=arm64 live-…` on the Mac host (15e).

## Wi‑Fi (15f)

Same priority as x86 step 13: last; likely Linux firmware + driver through the shim.

## Milestones in this repo

| Step | Done when |
|------|-----------|
| **15a** (this doc) | Boot path documented; DTB platform probe in kernel + host test |
| **15b** | m1n1 (or Asahi chain) loads MyOS; serial or fb text on real M1 |
| **15c** | See `myos-root` on Apple NVMe; USB installer |
| **15d** | Built-in kbd + panel enough for login |
| **15e** | aarch64 live userland like x86_64 stick |
| **15f** | Wi‑Fi on Apple hardware |

## References

- [Asahi Linux](https://asahilinux.org/) — boot chain, DT, drivers
- [m1n1](https://github.com/AsahiLinux/m1n1) — first-stage loader experiments
- Linux `Documentation/devicetree/bindings/arm/apple/` — compatible strings
- MyOS `arch/aarch64/entry.S` — x0 DTB convention (already matches Linux)
