/*
 * uaccess_test.c — the armed sleep inside copy_to_user / copy_from_user.
 * Only compiled into `make test` builds. See include/uaccess_test.h.
 */
#include <thread.h>
#include <uaccess_test.h>

/* Armed for one thread only. A global one-shot would be consumed by whatever
   copy happened to run first, which on this kernel is usually the console
   draining its ring — and the test would then pass without ever having
   produced the interleaving it is named after. */
static struct thread *armed_for;
static int countdown;

void uaccess_test_arm(int n)
{
    armed_for = n > 0 ? thread_current() : 0;
    countdown = n;
}

void uaccess_test_pause(void)
{
    if (armed_for != thread_current() || --countdown > 0)
        return;
    armed_for = 0;
    /* The sibling runs here. Interrupts are off and we are inside a syscall,
       which is exactly the situation wq_sleep leaves the kernel in when a
       page fault goes to the disk, so this is faithful rather than merely
       convenient. */
    thread_yield();
}
