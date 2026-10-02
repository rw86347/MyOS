#ifndef SELFTEST_H
#define SELFTEST_H

/* Boot-time self-tests — compiled only into `make test` builds (SELFTEST).
   check() counts results and prints only failures; selftest_summary() prints
   "Self-tests: N passed, M failed" (make test looks for "0 failed"). */
void check(int ok, const char *what);
void selftest_summary(void);

/* Run all the self-tests. Ends with the slot scheduler started. */
void run_selftests(void);

/* Handle a test key (F10/F11/F12 crash on purpose); 1 if it was one. */
int selftest_key(int key);

/* initrd_test.c */
void initrd_selftest(void);

/* bcache_test.c */
void bcache_selftest(void);

#endif
