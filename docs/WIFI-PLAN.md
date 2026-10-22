# Wi-Fi on MyOS — the plan (roadmap step 13)

**Goal:** the MSI Katana's Intel AX201 joins a WPA2/WPA3 network. A
Windows-style Wi-Fi icon in the taskbar tray lists the networks around and
connects to one.

**The rule (Rodger, 2026-10-06): the kernel is ours; the interfaces are
Linux's.** MyOS copies Linux's *interfaces* — the API a driver calls, the ABI
a program uses — and implements them itself. Linux code enters only as a
**driver** (Intel's `iwlwifi`), never as a part of the kernel. So the 802.11
stack is ours, written to Linux's interfaces on both sides:

```
 user    myos-wifi (ours: the tray icon) --control socket--> wpa_supplicant (Alpine, unchanged)
                                                                  |  nl80211 (generic netlink)
 ----------------------------------------------------------------------------------------------
 kernel  MyOS 802.11 stack (ours)                                 |
           up:   nl80211 — the same messages Linux's cfg80211 answers  <----+
           down: mac80211's driver API — ieee80211_ops, ieee80211_rx(), ieee80211_tx_status()...
         iwlwifi (Intel's driver, Linux source, unchanged; BSD-3-Clause terms)
         the shim (ours): the rest of the Linux driver API — PCI, MSI-X, DMA,
           firmware, sk_buff, net_device, NAPI, workqueues, timers, locks, RCU
         MyOS: local APIC, PCI, page allocator, network stack, AF_NETLINK
 hw      AX201 (CNVi, PCI 00:14.3 8086:43f0, MSI-X) + firmware iwlwifi-QuZ-a0-hr-b0-NN.ucode
```

FreeBSD does exactly this: its LinuxKPI runs the same `iwlwifi` on FreeBSD's
own 802.11 stack (net80211). Hardware facts:
[hardware-msi-katana-gf66.md](hardware-msi-katana-gf66.md).

**What "the same interface" means:**
- Our headers, Linux's names and paths (`include/linux/…`, `include/net/
  mac80211.h`, as `fb.h`/`io.h` already are), with the struct members the
  driver touches laid out as Linux has them. Written by us, not copied.
- `iwlwifi`'s `.c` files unchanged. A patch we cannot avoid lives in one
  folder as a `.patch` with its reason, so a newer `iwlwifi` is a re-copy.
- Programs see Linux's ABI: nl80211 and rtnetlink over `AF_NETLINK`, so
  Alpine's `iw`, `wpa_supplicant`, `wpa_cli` and `ip` run unchanged.

**Licence.** Nearly all of `iwlwifi` is "GPL-2.0 OR BSD-3-Clause"; we take it
under BSD-3-Clause (keeping its notices), as FreeBSD does, and use no file
that is GPL-only. No GPL code is in the kernel, so MyOS's own licence is a
free choice.

## Testing without the laptop

QEMU has no Wi-Fi card, so we build a **simulated radio** of our own — a
small driver written against the same mac80211 driver API (Linux has one
too, `mac80211_hwsim`; ours is original). Two simulated radios share a
simulated "air": one is a station, the other an access point. That tests
our 802.11 stack, nl80211, `wpa_supplicant`, DHCP and the tray icon in
`make test-wifi`. Only `iwlwifi` itself needs the MSI.

## Steps

Each step ends with something visible in QEMU, except W7.

- [x] **W0a. MSI and MSI-X** (2026-10-06). The local APIC (`arch/*/apic.c`,
  xAPIC or x2APIC) switched on when a driver first asks; the 8259 path stays
  as the firmware set it. IRQs 16-31 = vectors 48-63, acknowledged at the
  APIC. `pci_find_capability`, `pci_alloc_irq_vectors` (MSI-X, MSI, then the
  line), `pci_irq_vector`, 64-bit BARs. *Check:* `make ARCH=x86_64
  test-uefi-shell` reads a FAT32 partition from QEMU's NVMe on `MSI-X irq 16`,
  its registers at 768 GiB.
- [x] **W0b. AF_NETLINK** (2026-10-06). `net/netlink.c`: rtnetlink dumps of
  links, IPv4 addresses and routes; generic netlink with its controller
  (`nlctrl`, family lookup by name or id); SO_PASSCRED credentials for GLib.
  `net/dev_ioctl.c`: the interface ioctls (SIOCGIFINDEX, SIOCGIF/SIFFLAGS,
  HWADDR, MTU, ADDR, NETMASK, CONF...) on any socket, as wpa_supplicant and
  musl's if_nametoindex use them. *Check:* `make test-shell` greps `ip addr`
  (eth0 10.0.2.15/24) and `ip route` (default via 10.0.2.2); systest checks
  the raw messages on both CPUs; the desktop still logs in.
- [x] **W0c. Firmware files** (2026-10-06). `kernel/firmware.c`,
  `include/linux/firmware.h`: `request_firmware`, `firmware_request_nowarn`,
  `release_firmware` look in `/lib/firmware/updates/` then `/lib/firmware/`.
  A file still in the initramfs archive is lent with no copy; one on a disk
  (or changed since boot) is copied once. *Check:* `make test` self-test
  reads `myos-test.fw` "from the archive (no copy)" and gets ENOENT for a
  missing name. Plain files only: the live disk must unpack `.zst` ones.
- [x] **W1. The shim core** (the Linux driver API iwlwifi needs below
  802.11). In three parts:
  - [x] **W1a. Kernel services** (2026-10-06): `include/linux/` types,
    compiler, kconfig (real IS_ENABLED), list, bitops, bitfield, byteorder,
    unaligned, err, errno, printk, atomic, refcount/kref, spinlock (= irqs
    off: one CPU, kernel code never preempted), mutex, rcupdate (Tiny RCU,
    deferred frees on system_wq), completion, wait, jiffies (a real
    variable, counted by the tick), ktime (cycle counter), delay, timer,
    workqueue, slab/gfp, string, module/init/export. `kernel/linux_lib.c`
    (vsnprintf with %pM/%*ph/%pV..., printk, kstrto*, bitmaps, delays),
    `linux_slab.c`, `linux_sync.c`, `timer_list.c` (thread "ktimers",
    sorted list: the tick does no timer work), `workqueue.c` (one thread per
    queue, "events" = system_wq). *Check:* `make test`, 15 shim checks.
  - [x] **W1b. Devices** (2026-10-06): `linux/device.h` (devm_* released
    newest first), `linux/pci.h` (id tables, pci_register_driver → probe,
    config byte/word/dword, BAR sizing, pcim_iomap, MSI/MSI-X wrappers;
    struct pci_dev carries Linux's fields, the native bus number became
    `busnr`), `linux/interrupt.h` (request_threaded_irq: a hard handler in
    the interrupt, the thread part in "irq/N-name"; disable_irq masks an
    8259 line, replays a missed MSI), `linux/dma-mapping.h` (physical =
    device address, no IOMMU; coherent memory zeroed; dma_pool), pages
    (`struct page *` = its direct-map address, owners = pmm share counts).
    The shim's kmalloc above 1 KiB is a physically contiguous page run,
    as Linux's is (kheap.c `kmalloc_contig`). *Check:* `make test` drives
    QEMU's edu card through a driver written only to Linux's API: probe,
    BAR size, registers, MSI → hard + threaded handler, DMA both ways,
    remove + devres.
  - [x] **W1c. Packets** (2026-10-06): one sk_buff for the stack and the
    shim, in Linux's layout (next/prev first, cb[48], header offsets, the
    shared info after the data with up to 17 page frags, clones sharing the
    block through dataref; blocks over 1 KiB contiguous). `linux/skbuff.h`
    (queues, put/push/pull/trim, frags, skb_add_rx_frag, clone, copy,
    pskb_expand_head, linearize, header_pointer), compound pages (the last
    put_page frees 2^n frames). `linux/netdevice.h` (alloc_netdev with an
    aligned private area, register_netdevice — listed, still down —, dev_open,
    stopped queues, carrier, NAPI on a dummy device, rtnl), etherdevice,
    random. Native: alloc_skb(size, gfp), eth_type_trans returns __be16,
    netif_napi_add(dev, napi, poll) + _weight, ifindex slots never reused.
    *Check:* `make test`, 19 checks, every skb and page given back.
- [ ] **W2. Our 802.11 stack, station side:** scan (done: W2a-c), authenticate, associate,
  keys, data frames (802.11 <-> Ethernet), nl80211 for all of it.
  *Check:* `iw dev wlan0 scan` lists the simulated access point.
  - [x] **W2a. The 802.11 interface headers; iwlwifi compiles** (2026-10-06).
    Ours: `linux/ieee80211.h` (frame formats and the standard's numbers,
    HT/VHT/HE/EHT/UHR capabilities), `net/cfg80211.h` (wiphy, bands,
    channels, scan requests, regulatory calls), `net/mac80211.h` (hw, vif,
    sta, txq, tx/rx info in skb->cb, the 76 callbacks iwlwifi provides, the
    core's functions it calls) — only what is used, every number checked
    against Linux's. Copied (ISC): `net/regulatory.h`,
    `net/ieee80211_radiotap.h`, the nl80211 UAPI. Driver-local, ours:
    `iwl-debug.h`, `iwl-devtrace.h`, `mvm/rs.h` + `mvm/rs-myos.c`,
    `mei/iwl-mei.h`. Plus the kernel API the driver reaches for (scatter
    lists, typed kzalloc_obj, overflow-safe sizes, cleanup guards, PTP and
    DMI types...), with Linux's types (`u64` is `long long`, `PAGE_SIZE`
    unsigned long) and its flags for drivers (`-funsigned-char`).
    *Check:* `tools/iwlwifi-check.py`: 72 of 72 files compile with
    `-Wall`, no warnings.
  - [x] **W2b. The core and the simulated radio** (2026-10-06).
    `net/wireless/`: one core where Linux has two (cfg80211 + mac80211 —
    we have only soft-MAC drivers, so no cfg80211_ops layer). `core.c`:
    ieee80211_alloc_hw / register_hw (wiphy + core + driver data in one
    block; phyN; a `wlan%d` station interface appears), wlan0 up/down →
    driver start/add_interface and back, received frames (from the
    driver's interrupt they queue for a work thread), wiphy_work.
    `scan.c`: hardware scans only (both our drivers scan in the radio):
    probe-request elements per band (rates, HT, VHT), the scan request
    handed down, the end reported by the driver; the table of networks
    (64 per radio, channel taken from the AP's own DS/HT element, oldest
    replaced). `util.c`: header lengths, channel ↔ frequency, element
    search and splitting. `drivers/net/wireless/sim.c` (test builds): a
    radio with 13 + 4 channels and an "air" of two APs that answer its
    scans. *Check:* `make test` (i386), 10 checks: wlan0 appears, comes
    up, a 17-channel scan finds MyOS-Test (ch 6, −42 dBm, WPA2) and
    MyOS-Open-5G (ch 36, −61 dBm, open), a second scan is refused while
    one runs, down stops the driver.
  - [x] **W2c. nl80211, scanning** (2026-10-06). `net/wireless/nl80211.c`:
    the family "nl80211" (id 0x13; groups config, scan, regulatory, mlme,
    vendor, nan): GET_PROTOCOL_FEATURES, GET_WIPHY (one message, or split
    per band when the program can merge them), GET_INTERFACE,
    SET_INTERFACE (station only), TRIGGER_SCAN / ABORT_SCAN, GET_SCAN, and
    the events TRIGGER_SCAN / NEW_SCAN_RESULTS / SCAN_ABORTED on "scan".
    `net/netlink.c` gained the kernel side of generic netlink for other
    files (`include/genetlink.h`: message builder, replies, multicast).
    Fixed on the way: an ack now leaves in a datagram of its own, as
    Linux's does (libnl stops reading a datagram at the answer and then
    waits for the ack). The simulated radio dwells 10 ms per channel, as
    a real one does — iw listens for "scan done" only after asking.
    *Check:* `make test` (196): rawsystest drives nl80211 by hand (family,
    groups, interfaces, scan, events, results), then **Alpine's iw,
    unchanged**, runs `iw dev` and `iw dev wlan0 scan` and prints both
    simulated networks with frequency, signal, rates and RSN.
- [x] **W3. WPA2 in QEMU.** The simulated access point (a test AP in the
  simulated air), `wpa_supplicant` joins, DHCP on `wlan0`, `ping` across.
  (Checked by `make test`; no separate `make test-wifi` was needed.)
  - [x] **W3a. Joining a network** (2026-10-06). `net/wireless/mlme.c`:
    connect = find it (scan first if needed; the strongest AP of that
    SSID), a channel context for it, the AP as a peer, Authentication
    (open system) and Association, each tried 3 times 300 ms apart; the
    driver told every step as mac80211 tells it (sta_state NOTEXIST →
    NONE → AUTH → ASSOC → AUTHORIZED for an open network, chanctx
    add/assign, bss/vif_cfg/link_info_changed, mgd_prepare/complete_tx).
    Received management frames go to the MLME as wiphy work. Leaving:
    deauthenticate, undo it all. nl80211: CONNECT, DISCONNECT,
    GET_STATION, the CONNECT/DISCONNECT events on "mlme", the
    associated BSS marked in scan results, SSID and channel in the
    interface. The simulated radio has an AP side: on its channel, the AP
    answers Authentication and Association after 2 ms of "air".
    *Check:* `make test` (199): the kernel joins and leaves MyOS-Open-5G
    (AID 1, the driver's peer authorized, then gone), and Alpine's iw runs
    `connect -w`, `link` (Connected to 02:00:00:00:0a:02, SSID, 5180 MHz)
    and `disconnect`.
  - [x] **W3b. Data and IPv4 over Wi-Fi** (2026-10-06). `net/wireless/
    data.c`: Ethernet ↔ 802.11 data frames (to-DS out, from-DS in,
    RFC 1042 LLC/SNAP), to the driver's queue for the priority (the TXQ
    model iwlwifi uses: wake_tx_queue / ieee80211_tx_dequeue) or its
    tx(); only EAPOL passes until the link is authorized. IPv4 runs on
    one device at a time — a laptop's case: a link that becomes usable
    takes it over (each device keeps its lease; DHCP in a thread of its
    own), and when it goes IPv4 returns to one that is up. The simulated
    AP has a network behind it: ARP, a DHCP server (192.168.77.10), ping.
    *Check:* `make test` (202): the kernel gets the lease, pings
    192.168.77.1, and is back on eth0's lease after leaving; from user
    space, `ifconfig wlan0` shows 192.168.77.10 and busybox `ping`
    answers in 4 ms.
  - [x] **W3c. WPA2-Personal with wpa_supplicant** (2026-10-06).
    `net/wireless/key.c`: the keys wpa_supplicant installs go to the
    driver (set_key) — pairwise and group, CCMP; outgoing frames carry the
    key (and a CCMP header if the driver wants it from us), incoming ones
    were decrypted by the hardware. No software crypto: a driver that
    can't take keys can't do WPA2. nl80211: NEW_KEY / DEL_KEY / SET_KEY,
    SET_STATION (authorized: the port opens, IPv4 starts), and the
    harmless ones wpa_supplicant sends at start (REGISTER_FRAME,
    SET_POWER_SAVE, SET_CQM, SET_MULTICAST_TO_UNICAST). `net/packet.c`:
    AF_PACKET sockets — how wpa_supplicant sends and receives EAPOL.
    `/dev/random` and `/dev/urandom` (it reads its nonces there). EAPOL
    from the AP is let through while associating: the AP sends the first
    handshake message right behind its association answer (Linux's
    mac80211 does the same). The simulated AP is a WPA2 authenticator
    (test builds only, `drivers/net/wireless/sim_crypto.c`: SHA-1,
    HMAC, PBKDF2, the 802.11 PRF, AES and AES key wrap, checked against
    published test vectors): the 4-way handshake, the GTK, and frames
    "encrypted" — delivered only if the hardware holds the AP's key.
    *Check:* `make test` (207): **Alpine's wpa_supplicant 2.10,
    unchanged**, joins MyOS-Test with its passphrase ("Key negotiation
    completed [PTK=CCMP GTK=CCMP]", CTRL-EVENT-CONNECTED), DHCP leases
    192.168.77.10 over the protected link (its broadcasts use the group
    key), ping answers, and stopping wpa_supplicant leaves the network.
- [x] **W4. The tray icon** (`live/myos-wifi/`): signal bars in IceWM's tray
  (off / searching / 1-4 bars), click for the list (name, lock, bars),
  Connect asks for the password, like Windows. Talks to wpa_supplicant's
  control socket. *Check:* a QEMU screendump with the simulated network.
  - [x] The kernel side (2026-10-06): Unix datagram sockets (`net/unix.c`),
    which the control socket is. *Check:* `make test`: boottest's datagram
    checks, and Alpine's `wpa_cli status` says COMPLETED, MyOS-Test,
    WPA2-PSK.
  - [x] The program (2026-10-06): `live/myos-wifi/myos-wifi.c`, GTK3 and
    GtkStatusIcon; its own small control-socket client (commands on one
    socket, events on an attached second one). Bars drawn with cairo;
    the list scans when it opens; a secured network asks for its key and
    says when it was wrong (and forgets it); "Connect automatically"
    saves it (SAVE_CONFIG). Built for x86_64; started by IceWM's startup.
  - [x] On the desktop (2026-10-06): myos-menu is the tray's
    notification area now (it owns `_NET_SYSTEM_TRAY_S0` and takes the
    icon in where the theme had a picture of one; IceWM's own tray is
    off); GTK 3 moved into the RAM login image (it had none, so no GTK
    program could start there); nl80211 answers GET_SURVEY, so
    wpa_supplicant's SIGNAL_POLL works and the icon shows the real
    signal. `make WIFISIM=1` builds a kernel with the simulated radio
    registered at boot (test code, its own build folder). *Check:*
    `make ARCH=x86_64 WIFISIM=1 test-wifi-desktop` logs in, opens the list
    from the tray (MyOS-Test secured, 4 bars; MyOS-Open-5G open, 3 bars),
    joins MyOS-Test with its key, and the kernel gets 192.168.77.10; the
    screenshots are in `build/x86_64/wifisim/wifi-*.png`.
- [x] **W5. The live disk** (2026-10-06): `wpa_supplicant`, `iw`,
  `wireless-regdb` in a [wifi] group of `live/packages.x86_64.txt` (also
  in the RAM login image); the AX201's firmware, all three revisions, and
  its licence from linux-firmware 20260916 (`tools/fetch-firmware.sh`,
  pinned by SHA-256) in `/lib/firmware`; `/etc/wpa_supplicant/
  wpa_supplicant.conf` (control by the netdev group, which every account
  gets; networks saved there); a runit service that waits for wlan0;
  the tray icon in IceWM's startup. `live/tools/fetch-live.py --add` pins new
  packages without re-resolving the rest. *Check:* `make ARCH=x86_64
  test-uefi-login` still reaches the X login screen. A machine's own
  network can go in `live/overlay.local/` (never committed): the image
  then joins it at boot.
- [x] **W6. iwlwifi through the shim** (2026-10-06): all 72 files compile
  and link into the x86_64 kernel (`IWLWIFI=1`, the default there; 530 KB
  of code), and its init runs at boot after the root file system is up;
  in QEMU it registers its PCI driver and finds no card. What it needed
  that the shim lacked (64 symbols): the rest of mac80211's driver calls
  (`net/wireless/drvapi.c`: finding the AP, iterating keys, losing the
  connection, a firmware restart, and the access-point/TDLS/block-ack
  calls that can't happen in station mode), the country's rules
  (`net/wireless/reg.c`: the firmware's regulatory domain applied to the
  channels), firmware loading in the background
  (request_firmware_nowait), PCI recovery (function-level reset, remove
  and rescan, reprobe), bsearch, CRC-32, the Internet checksum. Only AES
  ciphers are offered (no TKIP/WEP). *Check:* `make ARCH=x86_64
  test-uefi-shell` boots with it and prints "wifi: iwlwifi built in".
- [ ] **W7. iwlwifi on the MSI.** Firmware loads, scan, join the home
  network. Real hardware only.
  The GPD Pocket 4's AX210 (8086:2725, `iwlwifi-ty-a0-gf-a0-89.ucode` +
  `.pnvm`) is on the live disk too (2026-10-21); not yet run.
- [ ] **W8. Remote control over Wi-Fi.** A small agent on MyOS connects out
  to the Mac: commands, kernel log, screenshots from `/dev/fb0`. Then
  network boot, so a new kernel needs no USB stick.

## iwlwifi, audited (Linux 7.2.9, 2026-10-06)

`build/ext/linux-7.2.9/drivers/net/wireless/intel/iwlwifi` (kernel.org
tarball). The AX201 uses the **mvm** op mode (not `mld`, which is for the
Wi-Fi 7 chips): `pcie/`, `pcie/gen1_2/`, `mvm/`, `fw/`, `fw/api/`,
`cfg/22000.c`, `cfg/rf-hr.c` and the top-level core — all "GPL-2.0 OR
BSD-3-Clause", except these, which we replace with our own:

| Linux file (GPL-2.0-only) | What it is | Ours |
|---|---|---|
| `iwl-debug.h` | the IWL_ERR / IWL_DEBUG_* macros | our header, same names |
| `iwl-devtrace*.{c,h}` | tracepoints (only with tracing on) | empty stand-ins |
| `mvm/rs.c`, `mvm/rs.h` | rate selection in the driver | `mvm/rs.h` + `rs-myos.c`: the AX201 picks rates in firmware (`rs-fw.c`, dual) |
| `mei/iwl-mei.h` | sharing the card with Intel's ME (AMT) | ours: "no ME", as Linux without CONFIG_IWLMEI |

Left out entirely: `dvm/` and `cfg/{1000,2000,5000,6000}.c` (older chips),
the rest of `mei/`, `mvm/vendor-cmd.c`, the KUnit tests. (`mvm/testmode.h`
turned out to be dual BSD/GPL, under an old-style notice: copied.)

PCI id `8086:43F0` matches `iwl_qu_long_latency_mac_cfg`. Firmware API 77
exactly; the file depends on the chip revision read at run time, so the live
disk carries all three: `iwlwifi-Qu-b0-hr-b0-77.ucode`,
`iwlwifi-Qu-c0-hr-b0-77.ucode`, `iwlwifi-QuZ-a0-hr-b0-77.ucode`.

## Decisions

- 2026-10-06: the kernel is ours; Linux's interfaces, not Linux's code.
  Linux drivers only, under their BSD terms where they have them.
- Downloads approved (2026-10-06): `iwlwifi` source (kernel.org, Linux
  7.2.x), the AX201 firmware (linux-firmware), Alpine's `wpa_supplicant`,
  `iw`, `wireless-regdb`, `hostapd`.
- Open: MyOS's licence (no GPL code forces one now).

## Size, honestly

Our 802.11 station stack is the largest original piece: FreeBSD's 802.11
glue for LinuxKPI is about 10,000 lines on top of a full 802.11 stack. Ours
is one chip, station mode first, one CPU — smaller, but still months, most
of it testable in QEMU with the simulated radio.
