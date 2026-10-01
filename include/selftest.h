#ifndef SELFTEST_H
#define SELFTEST_H

/* Boot-time self-tests. check() counts results and prints only failures,
   so everything fits on the screen; selftest_summary() prints one line:
   "Self-tests: N passed, M failed" (make test looks for "0 failed"). */
void check(int ok, const char *what);
void selftest_summary(void);

#endif
