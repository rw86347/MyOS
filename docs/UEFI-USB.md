# Write the UEFI image to a USB stick

The file to write for a real UEFI PC:

    /Users/rwilson/Dropbox/dev/MyOS/build/myos-live-uefi.img

About 1.5 GiB. GPT with a FAT32 ESP plus ext2 `myos-root`.
`make test-uefi` and `make test-live-uefi` cover QEMU+OVMF.
`make test` (Pentium BIOS, 64 MiB, `-cpu pentium3`) is a separate
image and must stay green. Rebuild with `make live-uefi`.

The small RAM-only image is still `build/myos-uefi.img`.

## Do not use BalenaEtcher

Etcher fails on this file (`requestMetadata is not a function`).
In Terminal, with the stick as `disk6` (check `diskutil list`):

```
diskutil unmountDisk /dev/disk6
sudo dd if=/Users/rwilson/Dropbox/dev/MyOS/build/myos-live-uefi.img of=/dev/rdisk6 bs=4m status=progress && sync && diskutil eject /dev/disk6 && echo WRITE_DONE
```

Do not pick `build/myos.img` or `build/myos-live.img` for a UEFI-only PC.
Those are BIOS/MBR disks; the firmware will skip them.

## What the colours mean

The loader paints a 48-pixel bar on the GOP instead of printing after
it has the memory map (ConOut can hang ExitBootServices):

- **Blue** — about to call ExitBootServices. If it stays blue, firmware
  did not return.
- **Gold** — left UEFI, jumping to the 32-bit kernel.
- **Red** — ExitBootServices failed.
- Kernel text, then the Golden Hour login — that is success.

A 32-bit pointer cannot hold the GOP at `0x400000000`. The kernel
reaches it with PSE-36 (extra physical bits in the page directory).
X draws in a low RAM copy; the kernel copies that onto the real bar.

## Boot the PC

Plug the stick in, power on, open the boot menu (often F12, F10, Esc).
Choose the USB entry that says UEFI, not "legacy".

This kernel has no USB mass-storage driver. On a laptop whose only
"disk" is the stick, `myos-root` is not mounted. The ESP's initramfs
is then a login-only userland: runit starts `myos-login` (X on
`/dev/fb0`). Firefox and the rest stay on the invisible ext2
partition. The framebuffer above 4 GiB is mapped with PSE-36.

The empty live password is just Enter.
