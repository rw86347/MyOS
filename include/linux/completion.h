/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_COMPLETION_H
#define LINUX_COMPLETION_H

/* "Wait until this is done": one thread waits, another (or an interrupt)
   says complete(). Counts, so a complete() before the wait isn't lost. */
#include <linux/types.h>
#include <thread.h>

struct completion {
    unsigned int done;
    struct wait_queue wait;
};
#define COMPLETION_INITIALIZER(w) { 0, { NULL } }
#define DECLARE_COMPLETION(w)     struct completion w = COMPLETION_INITIALIZER(w)
#define DECLARE_COMPLETION_ONSTACK(w) DECLARE_COMPLETION(w)
static inline void init_completion(struct completion *c) { c->done = 0; c->wait.first = NULL; }
static inline void reinit_completion(struct completion *c) { c->done = 0; }
void complete(struct completion *c);
void complete_all(struct completion *c);
void wait_for_completion(struct completion *c);
/* Jiffies left (at least 1) if it completed, 0 if the time ran out. */
unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout);
int  wait_for_completion_interruptible(struct completion *c);
long wait_for_completion_interruptible_timeout(struct completion *c, unsigned long timeout);
bool try_wait_for_completion(struct completion *c);
bool completion_done(struct completion *c);

#endif
