# Running MyOS in UTM, and why

Day-to-day work happens on the command line: `make run`, `make test`,
`make test-shell`, `make test-hd`. They are faster, scriptable, and every
self-test and benchmark lives there. UTM is not a nicer front end for that and
should not be used as one.

There is exactly one thing UTM can do that the command line cannot, and it is
the reason this file exists.

## Why UTM: ICMP to the internet

QEMU's default network backend is SLIRP (`-netdev user`), a TCP/IP stack in
user space. TCP, UDP and DNS reach the internet through it. **ICMP to anywhere
except the gateway does not.** Forwarding an echo request means either a raw
socket or an unprivileged ICMP datagram socket, and libslirp does not use the
latter on macOS, so `ping 8.8.8.8` from the guest gets nothing back. This was
confirmed rather than assumed: a packet capture shows MyOS emitting three
correct echo requests and no reply arriving, while `wget http://example.com/`
in the same boot succeeds. The limitation is on the host side; MyOS is right.

UTM's `Shared` network mode is Apple's **vmnet**, a real NAT in the macOS
kernel — the same thing Docker and Podman use. It forwards ICMP, so
`ping google.com` works. QEMU can use it directly with `-netdev vmnet-shared`,
but not as a normal user:

```
qemu-system-i386: -netdev vmnet-shared,id=n0: cannot create vmnet interface:
general failure (possibly not enough privileges)
```

vmnet needs root or the `com.apple.vm.networking` entitlement. UTM is signed
with it. That is the whole reason to leave the command line.

vmnet also has no fixed addresses — it hands them out by DHCP — which is why
MyOS needed `net/dhcp.c` before this mode was any use at all. A VM in `Shared`
mode with the old hard-coded 10.0.2.15 had no working network.

## Building a VM

```sh
tools/mkutm.sh --installed --net shared build/myos-hd.img \
    "$HOME/Desktop/MyOS networking.utm" <fresh-uuid>
```

`--net` picks the mode:

| mode | QEMU backend | address | `ping google.com` |
|---|---|---|---|
| `shared` | `vmnet-shared` | DHCP, 192.168.64.x | works |
| `emulated` | `-netdev user` (SLIRP) | DHCP, 10.0.2.15 | no — ICMP not forwarded |
| `bridged` | `vmnet-bridged` | DHCP, from the real router | works, on the real LAN |

### A UEFI machine for trying the installer

```sh
make ARCH=x86_64 utm-install UTM_INSTALL_VM="$HOME/Desktop/MyOS UEFI Install.utm"
```

A q35 machine starting through UEFI, the live system as a USB stick on UTM's
USB 3 bus (started first), and an empty 32 GB NVMe disk, as on the GPD P2
Max. Start it, click **Install MyOS** in the Start menu, choose the NVMe
disk. Afterwards, in the VM's settings (Drives), move the NVMe disk above the
USB drive and it starts from the disk. (Removing the USB drive there would
delete the stick's image from the VM.)

How UTM wires USB, read from its own debug log (2026-10-17): with USB 3 on,
UTM passes QEMU `-usb`, which on q35 is an ICH9 EHCI with UHCI companions,
and puts its USB keyboard, mouse and tablet there; its NEC xHCI
(`usb-controller-0`) gets only the USB drives and its three USB-sharing
slots. MyOS drives xHCI only, so the keyboard was dead. `mkutm.sh` adds a
keyboard and a tablet of its own on the xHCI in the VM's extra QEMU
arguments, which UTM puts after its own devices: QEMU sends keys to the
keyboard made last and the pointer to the tablet the system reads, so
those are the ones used. The xHCI gets 8 ports, because with its 4 and six
devices QEMU would put some behind a hub, which MyOS can't drive yet.

Its CPU is QEMU's `max`, not `qemu64`: Claude Code is built for SSSE3,
SSE4.2 and POPCNT and stops with "Illegal instruction" without them.

### UTM keeps a loaded VM's settings in memory

Rewriting `config.plist` while UTM has the VM open changes nothing: the next
start uses the settings UTM read before (its debug log shows the old QEMU
arguments). Quit UTM, or remove the VM from UTM's list, before starting it
again.

### The MAC address must differ per VM

`mkutm.sh` derives it from the VM's UUID. A fixed MAC is a trap in `shared`
mode, because vmnet is **one network shared by every guest on the Mac**: two
MyOS VMs started together would have the same address and DHCP would give them
the same lease or neither. In `bridged` mode that collision would be on the
real LAN. Deriving it from the UUID keeps a rebuilt VM's address stable — UTM
sees the same machine and a DHCP server offers it the same lease — while
making two different VMs differ.

## Driving a VM from a script

`tools/mkutm.sh --debug-port 4444` adds a QEMU monitor on `127.0.0.1:4444` and
a serial console on `4445`. This took several wrong turns, all of which come
down to one fact:

**UTM runs QEMU inside the macOS App Sandbox.** It can only write inside the
VM bundle, and it cannot bind a unix socket at all. Three things were tried:

| channel | result |
|---|---|
| `-monitor unix:/tmp/.../monitor.sock` | `Failed to bind socket: Operation not permitted` |
| `-serial file:$HOME/.../serial.log` | `could not connect serial device to character backend` |
| `-monitor tcp:127.0.0.1:4444,server,nowait` | works |

So both channels go over localhost TCP. The monitor is confirmed working:

```sh
echo "info status" | nc -w 4 127.0.0.1 4444      # -> VM status: running
for k in $(tools/qemu-type.sh 'ls /'); do echo "$k"; sleep 0.04; done \
    | nc -w 3 127.0.0.1 4444                      # type a command
```

The serial console over TCP has so far produced nothing, and is unexplained:
the monitor on the adjacent port works, and the same kernel writes to serial
normally under plain QEMU (`make run`, `-serial stdio`). Not chased, because
there is a better way to get output (below).

### `utmctl` needs permission, and hangs until it gets it

`utmctl` is at `/Applications/UTM.app/Contents/MacOS/utmctl`. The first call
triggers a macOS automation prompt ("… wants access to control UTM"). **Until
that is approved the command hangs indefinitely** — no error, no timeout. It
hung for over two minutes before being killed, which looked like a broken tool
rather than a pending dialog. If `utmctl list` produces nothing, look for the
dialog.

Once approved:

```sh
utmctl list                        # UUID, status, name
utmctl start <uuid>
utmctl stop <uuid>
utmctl attach <uuid>               # serial i/o to this terminal
```

### UTM caches a VM by UUID, including its path

Rebuilding a bundle at a new path with the **same** UUID keeps the old
registration: `utmctl start` launched the previous bundle and failed with an
error naming a path that no longer existed in the config. Give every rebuilt
probe a fresh UUID, or expect to debug a VM you are not looking at.

### The reliable way to get output: put the test in the image

Serial over TCP did not work and driving a console through `sendkey` is slow
and fragile. What worked first time was baking the experiment into the probe
image's `/init`, so the machine runs it itself and prints to its own console:

```sh
echo "=== PROBE: ping google.com ==="
ping -c 3 -W 4 google.com 2>&1 | tail -4
```

Build, boot, read the screen. No channel to the host needed. A probe image is
disposable, so patch `initramfs/init`, `make hd`, build the VM, and revert.

## What this verified

A probe VM in `shared` mode, booted 2026-10-04:

```
e1000: 5a:4d:59:a5:1d:6e bar 0xfebc0000 irq 10 link up
dhcp: lease from 192.168.64.1
ipv4: eth0 is 192.168.64.12/24 gateway 192.168.64.1 dns 192.168.64.1
nameserver 192.168.64.1                     <- /etc/resolv.conf, from /proc/net/dns
2 packets transmitted, 2 packets received   <- the gateway
3 packets transmitted, 3 packets received   <- 8.8.8.8
3 packets transmitted, 3 packets received   <- google.com, 260/282/311 ms
```

So: DHCP works against a real server (not QEMU's), the kernel's nameserver
reaches user space, and ICMP crosses a real NAT. It also closes the question
`docs/gaps.md` had been carrying — vmnet does forward ICMP.

One lookup failed in that boot — `wget` could not resolve `example.com` while
`ping google.com` resolved fine — and it did not reproduce in 32 further
attempts. What was tested, and the method that got the answer, is below.

## Reading results out of a VM you cannot talk to

Serial over TCP produced nothing and the sandbox blocks the easy channels, so
the way to get data out of a UTM guest is to have it write to its own disk and
read the disk from the host afterwards. A probe `/init` that runs the
experiment, writes `/root/probe.txt`, and calls `sync`:

```sh
qemu-img convert -U -f qcow2 -O raw "$VM/Data/"*.qcow2 /tmp/probe.img
S=$(( $(wc -c < build/myos.img) / 512 )); START=$(( (S + 2047) / 2048 * 2048 ))
dd if=/tmp/probe.img of=/tmp/root.img bs=512 skip=$START
printf 'cat /root/probe.txt\n' | debugfs /tmp/root.img
```

`-U` on `qemu-img` is needed because UTM keeps a lock on the image even after
the VM stops. And `debugfs` prints its prompt on the same line as the first
line of output, so filtering lines that start with `debugfs` silently eats the
first result — which cost a confusing few minutes.

A packet capture can go in the bundle too, via the VM's additional arguments:

```
-object filter-dump,id=d0,netdev=net0,file=<bundle>/Data/net.pcap
```

Two constraints, both learned the hard way: the file has to be inside the
bundle, because the sandbox refuses everywhere else, and **the path must not
contain a space** — UTM splits additional arguments on whitespace, so
`~/Desktop/MyOS probe.utm/...` arrives at QEMU as `probe.utm/...`. Name a
probe VM without spaces.

That probe crossed program against hostname — `ping`, `wget` and `nslookup`
against `example.com` and `google.com`, three rounds — and came back 18 of 18,
with every query answered in the capture. See `docs/gaps.md` for the full list
of what was ruled out.

## Native ARM64 (QEMU virt, HVF)

`make ARCH=arm64 utm-arm64` writes `build/arm64/MyOS-ARM64.utm` — aarch64,
QEMU virt, host CPU, kernel.elf, serial Terminal. Double-click the bundle
and type at the `bash-5.2#` prompt. This is the ARM64 bring-up kernel, not
the x86 portable tree; there is no virtio-net yet.
