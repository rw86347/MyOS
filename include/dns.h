/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/*
 * One A lookup over UDP to 10.0.2.3 (QEMU's DNS). The name is a hostname
 * like "example.com", not a binary label string. 0 and *ip set (host
 * order), or -errno.
 */
int dns_resolve(const char *name, uint32_t *ip);

#endif
