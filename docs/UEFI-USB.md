# Write the UEFI image to a USB stick

The file to write for the 64-bit hello (this test):

    /Users/rwilson/Dropbox/dev/MyOS/build/x86_64/myos-uefi.img

Also on the Desktop as `MyOS-x64-hello.img`. About 66 MiB. FAT32
ESP only: `BOOTX64.EFI`, `KERNEL.BIN`, `FONT.BIN`. Rebuild with
`make ARCH=x86_64`. QEMU+OVMF: `make ARCH=x86_64 test-uefi`.

The 64-bit live disk is `build/x86_64/myos-live-uefi.img` (~2.2 GiB,
Chromium, ELF64 myos-login). The kernel on that stick is still the
hello binary: you will see the gold bar and "Hello world", not the
login. Rebuild with `make ARCH=x86_64 live-uefi`.

The 32-bit live desktop image is still
`build/myos-live-uefi.img` (~1.5 GiB). `make test` (Pentium BIOS)
is a separate image.

## Do not use BalenaEtcher

Etcher fails on this file (`requestMetadata is not a function`).
In Terminal, with the stick as `disk6` (check `diskutil list`):

```
diskutil unmountDisk /dev/disk6
sudo dd if=/Users/rwilson/Dropbox/dev/MyOS/build/x86_64/myos-uefi.img of=/dev/rdisk6 bs=4m status=progress && sync && diskutil eject /dev/disk6 && echo WRITE_DONE
```

Do not pick `build/myos.img` or `build/myos-live.img` for a UEFI-only PC.
Those are BIOS/MBR disks; the firmware will skip them.

## What the colours mean

The loader paints a 48-pixel bar on the GOP instead of printing after
it has the memory map (ConOut can hang ExitBootServices):

- **Blue** — about to call ExitBootServices. If it stays blue, firmware
  did not return.
- **Gold** — left UEFI, jumping to the kernel.
- **Red** — ExitBootServices failed.
- **Dark screen, gold bar, "Hello world from MyOS!" / "long mode  x86_64"**
  — the 64-bit kernel painted the GOP. That is success for this image.
- Kernel text, then the Golden Hour login — that is the 32-bit live image.

A 32-bit pointer cannot hold the GOP at `0x400000000`. The kernel
reaches it with PSE-36 (extra physical bits in the page directory).
X draws in a low RAM copy; the kernel copies that onto the real bar.

## Boot the PC

Plug the stick in, power on, open the boot menu (often F12, F10, Esc).
Choose the USB entry that says UEFI, not "legacy".

Since 2026-10-07 the kernel has its own USB stick driver (xHCI + mass
storage, `docs/USB-PLAN.md`): it finds the stick again once it runs,
and the stick's ext2 `myos-root` becomes `/` — the whole live system,
and what you save stays on the stick. The boot screen shows `xhci:`,
`usb 1-N:`, `usb-storage: sda:` and `ext2: sda2 mounted at /`. If that
fails, the ESP's initramfs is still there as before: a login-only
userland in RAM (runit starts `myos-login`, X on `/dev/fb0`). The
framebuffer above 4 GiB is mapped with PSE-36.

The empty live password is just Enter.
