#ifndef UACCESS_TEST_H
#define UACCESS_TEST_H

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
 */

/* Arm the n'th copy from this thread onwards (n = 1: the very next one). */
void uaccess_test_arm(int n);

/* Called from copy_to_user / copy_from_user before the copy. */
void uaccess_test_pause(void);

#endif
