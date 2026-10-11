/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_LOCKDEP_H
#define LINUX_LOCKDEP_H
/* Linux's lock checker: nothing to check on one CPU here. */
#include <linux/spinlock.h>
struct lock_class_key { int unused; };
struct lockdep_map { int unused; };
#define lockdep_assert_irqs_disabled() do { } while (0)
#define lockdep_assert_in_softirq()    do { } while (0)
#define lockdep_init_map(m, n, k, s)   do { (void)(m); (void)(k); } while (0)
#define lockdep_set_class(l, k)        do { (void)(l); (void)(k); } while (0)
#define lock_map_acquire(m)            do { } while (0)
#define lock_map_acquire_read(m)       do { } while (0)
#define lock_map_release(m)            do { } while (0)
#endif
