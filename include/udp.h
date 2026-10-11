/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef UDP_H
#define UDP_H

#include <stdint.h>

/*
 * UDP. A bound port gets a callback from the receive path, with the ctx
 * given at bind time (a socket, or NULL). The callback must copy what it
 * wants and return: the bytes are gone when it returns, and it must not
 * send. At most 16 ports.
 */

typedef void (*udp_recv_fn)(void *ctx, uint32_t src, uint16_t sport,
                            const uint8_t *data, uint32_t len);

/* 0, -EADDRINUSE (another owner), or -ENOMEM (table full). */
int udp_bind(uint16_t port, udp_recv_fn fn, void *ctx);
void udp_unbind(uint16_t port);

/* A free port from 49152 up, or 0 if none. */
uint16_t udp_free_port(void);

/* sport is ours, dport is theirs. 0, or -errno. */
int udp_sendto(uint32_t dst, uint16_t sport, uint16_t dport,
               const void *data, uint32_t len);

void udp_input(uint32_t src, uint32_t dst, const uint8_t *pkt, uint32_t len);

#endif
