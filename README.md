# MyOS

**MyOS, written by Rodger Wilson.** Open source for your enjoyment: BSD 2-Clause licence (see `LICENSE`).

A small hobby operating system for old x86 (i686) and ARM machines, aiming to run Linux user-space programs
by implementing the Linux system call interface. See `docs/ROADMAP.md`.

```
make        # build
make run    # boot in QEMU
make debug  # boot paused, attach gdb to localhost:1234
```

The live and installer disks (the desktop, its programs, the image builders)
are a second repo, [myos-live](https://github.com/rw86347/myos-live), checked
out inside this one:

```
git clone https://github.com/rw86347/myos-live.git live
make ARCH=x86_64 live-uefi
```
