/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef FUZZ_TARGETS_H
#define FUZZ_TARGETS_H

#include <stddef.h>

struct fuzz_target {
    const char *name;
    void (*fn)(const unsigned char *data, size_t len);
};

/* The table of attack surfaces, chosen by name on the command line. */
const struct fuzz_target *fuzz_targets(unsigned *n);

/* Bring the stack up once, before any run. */
void fuzz_init(void);

/* One run: reset per-run state, then hand the bytes to the target. */
void fuzz_run(const struct fuzz_target *t, const unsigned char *data, size_t len);

/* Non-zero if the stack is holding more packets than it should. Checked
   between runs: a slow skb leak is a denial of service, and nothing else
   in the harness would notice it. */
int fuzz_leaked(void);

#endif
