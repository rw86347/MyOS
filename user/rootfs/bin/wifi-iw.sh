#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Alpine's iw, unchanged, against MyOS's nl80211 (docs/WIFI-PLAN.md W2c,
# W3a), on the simulated radio of `make test` kernels: wlan0 up, the
# interface list, a scan with both simulated networks, then joining the
# open one, its link, an address by DHCP, ping, and leaving it. Exit 0 if all is right.
ifconfig wlan0 up || exit 1
iw dev > /tmp/iw-dev.txt 2>&1
grep -q "Interface wlan0" /tmp/iw-dev.txt || { cat /tmp/iw-dev.txt; exit 2; }
iw dev wlan0 scan > /tmp/iw-scan.txt 2>&1 || { cat /tmp/iw-scan.txt; exit 3; }
grep -q "SSID: MyOS-Test" /tmp/iw-scan.txt || exit 4
grep -q "SSID: MyOS-Open-5G" /tmp/iw-scan.txt || exit 5
grep -q "freq: 2437" /tmp/iw-scan.txt || exit 6
grep -q "signal: -42.00 dBm" /tmp/iw-scan.txt || exit 7
grep -q "RSN:" /tmp/iw-scan.txt || exit 8
head -24 /tmp/iw-scan.txt
echo "wifi-iw: iw dev wlan0 scan lists MyOS-Test and MyOS-Open-5G"

# Join the open network and look at the link (W3a).
iw dev wlan0 connect -w MyOS-Open-5G > /tmp/iw-conn.txt 2>&1
cat /tmp/iw-conn.txt
grep -q "connected to 02:00:00:00:0a:02" /tmp/iw-conn.txt || exit 9
iw dev wlan0 link > /tmp/iw-link.txt 2>&1
cat /tmp/iw-link.txt
grep -q "Connected to 02:00:00:00:0a:02" /tmp/iw-link.txt || exit 10
grep -q "SSID: MyOS-Open-5G" /tmp/iw-link.txt || exit 11
grep -q "freq: 5180" /tmp/iw-link.txt || exit 12

# IPv4 over it: the AP's DHCP server leases .10, the AP answers ping (W3b).
for i in 1 2 3; do ifconfig wlan0 | grep -q "inet addr:192.168.77.10" && break; sleep 1; done
ifconfig wlan0 | grep "inet addr" || exit 15
ping -c 1 -W 2 192.168.77.1 > /tmp/ping.txt 2>&1 || { cat /tmp/ping.txt; exit 16; }
grep "bytes from" /tmp/ping.txt
iw dev wlan0 disconnect || exit 13
iw dev wlan0 link > /tmp/iw-link.txt 2>&1
grep -q "Not connected" /tmp/iw-link.txt || { cat /tmp/iw-link.txt; exit 14; }
echo "wifi-iw: connect, link, DHCP, ping, disconnect"
rm -f /tmp/iw-dev.txt /tmp/iw-scan.txt /tmp/iw-conn.txt /tmp/iw-link.txt /tmp/ping.txt
ifconfig wlan0 down
