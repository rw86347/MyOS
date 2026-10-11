#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# Echo server for user/netbench.c: QEMU's user network maps the guest's
# 10.0.2.2:7777 (UDP) and 10.0.2.2:7778 (TCP) to 127.0.0.1 here.
import socket
import threading

def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 7777))
    while True:
        data, addr = s.recvfrom(2048)
        s.sendto(data, addr)

def tcp_client(c):
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    while True:
        data = c.recv(4096)
        if not data:
            break
        c.sendall(data)
    c.close()

def tcp():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 7778))
    s.listen(4)
    while True:
        c, _ = s.accept()
        threading.Thread(target=tcp_client, args=(c,), daemon=True).start()

threading.Thread(target=udp, daemon=True).start()
tcp()
