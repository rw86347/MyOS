/*
 * user_test.c — run the embedded user programs and check what happened.
 *
 *   hello: prints with write(), gets -EFAULT for a kernel pointer, exit(42)
 *   peek:  reads kernel memory -> page fault -> killed (exit code 139),
 *          and the kernel keeps running
 *
 * Runs before the slot scheduler starts (round-robin mode): main yields
 * until the program's thread is dead.
 */
#include <kprintf.h>
#include <paging.h>
#include <selftest.h>
#include <thread.h>
#include <user_test.h>
#include <usermode.h>

extern const char user_hello[], user_hello_end[];   /* user/programs.asm */
extern const char user_peek[], user_peek_end[];

/* Run one program to completion; returns its exit code (-1 = didn't start). */
static int run(const char *name, const char *start, const char *end)
{
    struct thread *t = user_spawn(name, start, end - start);
    if (!t)
        return -1;
    while (t->state != THREAD_DEAD)
        thread_yield();
    user_cleanup();
    kprintf("User mode: %s exited with code %d\n", name, t->exit_code);
    return t->exit_code;
}

void user_selftest(void)
{
    check(run("hello", user_hello, user_hello_end) == 42,
          "user: hello wrote, got -EFAULT for a kernel pointer, exit(42)");
    check(run("peek", user_peek, user_peek_end) == 139,
          "user: reading kernel memory kills the program, not the kernel");
    check(vmm_translate(USER_CODE_BASE) == 0, "user: program memory freed");
}
