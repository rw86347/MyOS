# Chain-loading MyOS on Apple Silicon (m1n1)

Roadmap step **15b**: get early serial and a DTB-driven memory map before internal NVMe (15c).

## What you need

- **MacBook Air (M1)** or another M1-class machine (see `docs/hardware-apple-silicon.md`).
- A **second computer** (often another Mac) on USB or network to run the m1n1 client and see serial.
- **m1n1** built from [Asahi Linux m1n1](https://github.com/AsahiLinux/m1n1) (follow their install docs).
- MyOS payload: `make ARCH=arm64 m1n1-payload` → `build/arm64/m1n1/` (`kernel.bin`, `handoff.txt`).

## Hand-off contract (same as Linux arm64)

| Item | Value |
|------|--------|
| Load address | **0x40100000** (`ARM64_M1N1_KERNEL_LOAD`) |
| Entry | `_start` (see `handoff.txt` / `nm kernel.elf`) |
| **x0** | Physical address of the **hardware** device tree (FDT) |
| MMU | Off |
| EL | EL1 (MyOS drops from EL2 if needed) |

On success, serial should include a line like:

```text
Boot: x0=0x… dtb=0x… (x0) platform=Apple …
```

If you see `(embedded)` on real hardware, x0 was not set — fix the loader, not the kernel.

## Suggested first try on hardware

1. Boot the Mac into **m1n1** using Asahi’s documented flow (recovery / checkra1n-style chain varies by model — use current m1n1 README).
2. Copy `build/arm64/m1n1/kernel.bin` (or `kernel.elf`) to the machine that runs the m1n1 **client**.
3. From the m1n1 shell, chain-load MyOS at **0x40100000** with **x0 = DTB** (m1n1 commands change over time; match their docs for “load kernel + dtb”).
4. Watch **serial** (m1n1 proxy / USB debug cable). Expect: Apple platform banner, `FDT: magic ok`, RAM line from DTB, then initramfs/bash if UART and RAM are correct.

## QEMU vs hardware

| Build | Purpose |
|-------|---------|
| `kernel.elf` | Normal QEMU virt (embedded virt DTB when x0=0) |
| `kernel-apple-harness.elf` | CI: Apple DTB embedded when x0=0 |
| `kernel-m1n1-sim.elf` | CI: simulates m1n1 by stuffing harness DTB into x0 in `_start` |

Hardware always uses **`kernel.bin` from `m1n1-payload`** with the **firmware DTB in x0**.

## After first boot prints

1. **Real `apple,uart`** (or the SIO path your DTB exposes) — harness uses PL011 @ 0x09000000 for QEMU only.
2. **15c** — NVMe / root disk on internal SSD.
3. **15d** — built-in keyboard and panel.
4. **15e** — aarch64 live userland image on disk/USB.

Log milestones in `docs/LOG.md` when each works on the M1 Air.
