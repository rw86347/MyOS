/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef UACCESS_TEST_H
#define UACCESS_TEST_H

#include <stdint.h>

/*
 * uaccess_test.h — make a copy to or from user memory sleep on purpose.
 *
 * Only in `make test` builds. The reason it exists: `copy_to_user` can sleep,
 * because a fault on a page of an mmap'd file waits for the disk, and seven
 * real bugs came from code that assumed it could not (docs/security.md). Every
 * one of those fixes is an argument about an interleaving that had never been
 * produced on purpose — which, by the standard this project applies to
 * everything else, means none of them were tested.
 *
 * This produces it. When armed, the next copy by this thread yields the CPU
 * before copying anything. A yield is what a sleep looks like from the other
 * side: the sibling runs, sees whatever shared state we left behind, and acts
 * on it. Code that commits its ring index before copying survives; code that
 * commits afterwards hands the sibling a stale snapshot, which is the bug.
 *
 * It is a yield and not a real disk wait because it has to be deterministic:
 * the test wants the interleaving every time, not one time in a thousand.
 * One yield was not enough for that: the scheduler may run some other
 * thread and come straight back, and the test then failed without anything
 * being wrong (racetest's epoll check, a few times in a hundred). So the
 * test passes the address of its sibling's "done" flag, and the armed copy
 * keeps yielding until the flag is set (at most 200 ms).
 */

/* Arm the n'th copy from this thread onwards (n = 1: the very next one);
   it yields until the user word at `until` is non-zero (0: one yield). */
void uaccess_test_arm(int n, uintptr_t until);

/* Called from copy_to_user / copy_from_user before the copy. */
void uaccess_test_pause(void);

#endif
