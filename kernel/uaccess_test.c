/*
 * uaccess_test.c — the armed sleep inside copy_to_user / copy_from_user.
 * Only compiled into `make test` builds. See include/uaccess_test.h.
 */
#include <interrupts.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <uaccess_test.h>

/* Armed for one thread only. A global one-shot would be consumed by whatever
   copy happened to run first, which on this kernel is usually the console
   draining its ring — and the test would then pass without ever having
   produced the interleaving it is named after. */
static struct thread *armed_for;
static int countdown;
static uintptr_t until;                 /* a user flag: yield until it's set */

void uaccess_test_arm(int n, uintptr_t flag)
{
    armed_for = n > 0 ? thread_current() : 0;
    countdown = n;
    until = flag;
}

void uaccess_test_pause(void)
{
    if (armed_for != thread_current() || --countdown > 0)
        return;
    armed_for = 0;
    /* The sibling runs here. Interrupts are off and we are inside a syscall,
       which is exactly the situation wq_sleep leaves the kernel in when a
       page fault goes to the disk, so this is faithful rather than merely
       convenient. Until the sibling says it has acted: a yield may go to
       some other thread first. (Reading the flag is a copy too; disarmed
       above, so it doesn't come back here.) */
    uint32_t end = timer_ms() + 200, done = 0;
    do {
        thread_yield();
        if (until && get_user32(&done, until) < 0)
            break;
    } while (until && !done && (int32_t)(timer_ms() - end) < 0);
}
