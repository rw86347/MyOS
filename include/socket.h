/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include <syscalls.h>

/*
 * Sockets (net/socket.c): the Linux socket syscalls on top of our IPv4.
 * A socket is an open file, so read/write/poll/close/dup/fork all work on
 * it. AF_INET: SOCK_STREAM (TCP), SOCK_DGRAM (UDP) and SOCK_RAW with
 * IPPROTO_ICMP (what ping uses); AF_UNIX (unix.c); AF_NETLINK (netlink.c);
 * AF_PACKET (packet.c).
 * Struct layouts and numbers match Linux.
 */

/* Every ICMP packet that arrives, IP header first: raw sockets get a copy. */
void raw_icmp_input(const uint8_t *ip, uint32_t len);

/* SO_RCVTIMEO / SO_SNDTIMEO, for every kind of socket (socket.c). The
   option comes as a struct timeval: two longs (20, 21), or two 64-bit
   numbers (66, 67: what 32-bit musl, with its 64-bit time_t, tries
   first). Kept as ms, 0 = wait for ever. */
#define SO_RCVTIMEO_OLD 20
#define SO_SNDTIMEO_OLD 21
#define SO_RCVTIMEO_NEW 66
#define SO_SNDTIMEO_NEW 67
#define SO_IS_TIMEO(opt) ((opt) == 20 || (opt) == 21 || (opt) == 66 || (opt) == 67)
#define SO_IS_RCVTIMEO(opt) ((opt) == 20 || (opt) == 66)
int sock_timeo_set(uint32_t *ms, uint32_t opt, uintptr_t val, uint32_t len);
int sock_timeo_get(uint32_t ms, uint32_t opt, uintptr_t val, uintptr_t lenp);
/* When a wait with timeout ms ends (0: never), for sock_wait. */
uint32_t sock_deadline(uint32_t ms);
/* One wait for a socket call: -EAGAIN if nonblock or the deadline has
   passed; a signal: -ERESTARTSYS, or -EINTR when there is a deadline (Linux
   never restarts a socket call with a timeout); else sleep on q (until
   woken or the deadline) and 0: check again. Interrupts off. */
struct wait_queue;
int sock_wait(struct wait_queue *q, int nonblock, uint32_t deadline);

SYSCALL_DEF(sys_socket);
SYSCALL_DEF(sys_bind);
SYSCALL_DEF(sys_connect);
SYSCALL_DEF(sys_getsockname);
SYSCALL_DEF(sys_getpeername);
SYSCALL_DEF(sys_sendto);
SYSCALL_DEF(sys_recvfrom);
SYSCALL_DEF(sys_sendmsg);
SYSCALL_DEF(sys_recvmsg);
SYSCALL_DEF(sys_setsockopt);
SYSCALL_DEF(sys_getsockopt);
SYSCALL_DEF(sys_shutdown);
SYSCALL_DEF(sys_listen);
SYSCALL_DEF(sys_accept4);
SYSCALL_DEF(sys_socketpair);
SYSCALL_DEF(sys_socketcall);

/* net/unix.c: AF_UNIX sockets. socket.c sends a call here when the fd is
   one (unix_fd), with the same arguments. */
#define AF_UNIX 1
int unix_fd(uint32_t fd);
int32_t unix_socket(uint32_t type, uint32_t protocol);
int32_t unix_socketpair(uint32_t type, uint32_t protocol, uintptr_t usv);
SYSCALL_DEF(unix_bind);
SYSCALL_DEF(unix_listen);
SYSCALL_DEF(unix_connect);
SYSCALL_DEF(unix_accept4);
SYSCALL_DEF(unix_getsockname);
SYSCALL_DEF(unix_getpeername);
SYSCALL_DEF(unix_sendto);
SYSCALL_DEF(unix_recvfrom);
SYSCALL_DEF(unix_sendmsg);
SYSCALL_DEF(unix_recvmsg);
SYSCALL_DEF(unix_setsockopt);
SYSCALL_DEF(unix_getsockopt);
SYSCALL_DEF(unix_shutdown);

/* net/netlink.c: AF_NETLINK sockets (rtnetlink, generic netlink), routed
   the same way (netlink_fd). */
#define AF_NETLINK 16
int netlink_fd(uint32_t fd);
int32_t netlink_socket(uint32_t type, uint32_t protocol);
SYSCALL_DEF(netlink_bind);
SYSCALL_DEF(netlink_connect);
SYSCALL_DEF(netlink_getsockname);
SYSCALL_DEF(netlink_getpeername);
SYSCALL_DEF(netlink_sendto);
SYSCALL_DEF(netlink_recvfrom);
SYSCALL_DEF(netlink_sendmsg);
SYSCALL_DEF(netlink_recvmsg);
SYSCALL_DEF(netlink_setsockopt);
SYSCALL_DEF(netlink_getsockopt);
SYSCALL_DEF(netlink_unsupported);       /* listen, accept, shutdown */

/* net/packet.c: AF_PACKET sockets (link-layer frames: EAPOL, DHCP clients),
   routed the same way (packet_fd). packet_rcv sees every received frame. */
#define AF_PACKET 17
struct sk_buff;
int packet_fd(uint32_t fd);
int32_t packet_socket(uint32_t type, uint32_t protocol);
void packet_rcv(const struct sk_buff *skb);
SYSCALL_DEF(packet_bind);
SYSCALL_DEF(packet_getsockname);
SYSCALL_DEF(packet_sendto);
SYSCALL_DEF(packet_recvfrom);
SYSCALL_DEF(packet_sendmsg);
SYSCALL_DEF(packet_recvmsg);
SYSCALL_DEF(packet_setsockopt);
SYSCALL_DEF(packet_getsockopt);
SYSCALL_DEF(packet_unsupported);        /* connect, listen, accept, getpeername, shutdown */

#endif
