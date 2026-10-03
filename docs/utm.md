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

One thing failed in that boot and is still unexplained: `wget` could not
resolve `example.com` while `ping google.com` resolved fine. Four hypotheses
were tested from the command line and all came back clean — musl's dual
A+AAAA query, the per-socket receive queue depth, the vmnet resolver itself
(reachable from command-line QEMU through SLIRP's NAT, which is a useful trick
in itself), and the burst of pings that preceded it. Details and what is left
to try in `docs/gaps.md`.
