/*
 * selftest.c — tiny pass/fail counter shared by the boot-time tests.
 */
#include <kprintf.h>
#include <selftest.h>

static int passed, failed;

void check(int ok, const char *what)
{
    if (ok) {
        passed++;
    } else {
        failed++;
        kprintf("  FAIL: %s\n", what);
    }
}

void selftest_summary(void)
{
    kprintf("Self-tests: %d passed, %d failed\n", passed, failed);
}
