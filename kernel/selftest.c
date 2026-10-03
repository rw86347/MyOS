/*
 * selftest.c — the boot-time self-tests (only in `make test` builds).
 *
 * check() counts results and prints only failures, so everything fits on
 * the screen; selftest_summary() prints "Self-tests: N passed, M failed",
 * which `make test` looks for.
 */
#include <cycles.h>
#include <disk_demo.h>
#include <keyboard.h>
#include <kprintf.h>
#include <mem_test.h>
#include <sched_test.h>
#include <selftest.h>
#include <thread_test.h>
#include <timer.h>
#include <user_test.h>

static int passed, failed;

void pcache_selftest(void);
void fatvfs_selftest(void);
void net_selftest(void);

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

void run_selftests(void)
{
    kprintf("Cycle counter: %lu MHz\n", cycles_per_us());
    uint32_t t0 = timer_ms();
    sleep_ms(100);
    kprintf("Timer: %u Hz (sleep 100 ms = %lu ticks), keyboard ready\n",
            TIMER_HZ, timer_ms() - t0);

    initrd_selftest();      /* the initramfs arrived intact */
    mem_selftest();         /* first: it checks the heap is back to empty */
    disk_demo();
    bcache_selftest();      /* the block cache, on the test disk */
    pcache_selftest();      /* disk file pages: second read from RAM */
    fatvfs_selftest();      /* FAT32 files through the VFS */
    net_selftest();         /* loopback packet path, PCI e1000 */
    thread_selftest();      /* kernel threads + context switch */
    user_selftest();        /* ring 3 programs, syscalls, ELF, FPU */
    sched_selftest();       /* starts the 20 x 1 ms slot scheduler */
    selftest_summary();
}

/* F10/F11/F12: crash on purpose, to show the exception handlers work. */
int selftest_key(int key)
{
    if (key == KEY_F10) {
        kprintf("\n[F10] Reading through a NULL pointer...\n");
        volatile uint32_t *null = 0;
        kprintf("%lu\n", *null);           /* page 0 is not mapped */
    } else if (key == KEY_F12) {
        kprintf("\n[F12] Dividing by zero...\n");
        /* Written in assembly on purpose: in C, dividing by zero is
           "undefined behaviour", so GCC may assume it never happens and
           replace `1 / x` with a comparison — no divide, no fault. */
        __asm__ volatile ("mov $1, %%eax\n\t"
                          "xor %%edx, %%edx\n\t"
                          "xor %%ecx, %%ecx\n\t"
                          "div %%ecx"           /* EDX:EAX / 0 -> exception 0 */
                          : : : "eax", "ecx", "edx");
    } else if (key == KEY_F11) {
        kprintf("\n[F11] Loading an invalid segment selector...\n");
        __asm__ volatile ("mov $0x99, %%ax\n\tmov %%ax, %%ds" : : : "eax");
    } else {
        return 0;
    }
    return 1;
}
