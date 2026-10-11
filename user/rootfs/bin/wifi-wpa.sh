#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Alpine's wpa_supplicant, unchanged, against MyOS's nl80211 (docs/WIFI-PLAN.md
# W3c), on the simulated radio of `make test` kernels: it joins the WPA2
# network, runs the 4-way handshake with the simulated AP and installs the
# keys; then an address by DHCP and ping over the encrypted link, and
# stopping wpa_supplicant leaves the network. Exit 0 if all is right.
ifconfig wlan0 up || exit 1
mkdir -p /var/run
wpa_supplicant -i wlan0 -D nl80211 -c /etc/wpa-test.conf > /tmp/wpa.log 2>&1 &
wpa=$!
fail() { kill $wpa; wait $wpa; exit $1; }
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    grep -q CTRL-EVENT-CONNECTED /tmp/wpa.log && break
    sleep 1
done
cat /tmp/wpa.log
grep -q "CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:0a:01 completed" /tmp/wpa.log ||
    { fail 2; }
iw dev wlan0 link > /tmp/iw-link.txt 2>&1
cat /tmp/iw-link.txt
grep -q "Connected to 02:00:00:00:0a:01" /tmp/iw-link.txt || { fail 3; }
grep -q "SSID: MyOS-Test" /tmp/iw-link.txt || { fail 4; }

# wpa_cli over the control socket (Unix datagrams), as the tray icon uses it.
wpa_cli -i wlan0 status > /tmp/wpa-cli.txt 2>&1
cat /tmp/wpa-cli.txt
grep -q "^wpa_state=COMPLETED" /tmp/wpa-cli.txt || { fail 8; }
grep -q "^ssid=MyOS-Test" /tmp/wpa-cli.txt || { fail 9; }
grep -q "^key_mgmt=WPA2-PSK" /tmp/wpa-cli.txt || { fail 10; }
# The signal, as the tray icon asks for it (nl80211 GET_STATION + GET_SURVEY).
wpa_cli -i wlan0 signal_poll > /tmp/wpa-cli.txt 2>&1
cat /tmp/wpa-cli.txt
grep -q "^RSSI=-42" /tmp/wpa-cli.txt || { fail 11; }

# IPv4 over the encrypted link: the AP's DHCP server leases .10 (its
# broadcasts use the group key), the AP answers ping (the pairwise key).
for i in 1 2 3; do ifconfig wlan0 | grep -q "inet addr:192.168.77.10" && break; sleep 1; done
ifconfig wlan0 | grep "inet addr" || { fail 5; }
ping -c 1 -W 2 192.168.77.1 > /tmp/ping.txt 2>&1 || { cat /tmp/ping.txt; fail 6; }
grep "bytes from" /tmp/ping.txt

# Stopping wpa_supplicant disconnects.
kill $wpa
wait $wpa
iw dev wlan0 link > /tmp/iw-link.txt 2>&1
grep -q "Not connected" /tmp/iw-link.txt || { cat /tmp/iw-link.txt; exit 7; }
echo "wifi-wpa: WPA2 handshake, keys, DHCP, ping, disconnect"
rm -f /tmp/wpa.log /tmp/wpa-cli.txt /tmp/iw-link.txt /tmp/ping.txt
ifconfig wlan0 down
