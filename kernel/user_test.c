/*
 * user_test.c — run the test programs (from /bin in the initramfs) and
 * check what happened.
 *
 *   hello: prints with write(), gets -EFAULT for a kernel pointer, exit(42)
 *   peek:  reads kernel memory -> page fault -> killed (exit code 139),
 *          and the kernel keeps running
 *   args:  prints its argv from the start-up stack, exit(argc)
 *   fpu:   keeps values in FPU/SSE registers while being switched out;
 *          a kernel thread wipes those registers meanwhile, exit(0)
 *
 * Plus: the ELF loader must refuse broken files without crashing.
 *
 * Runs before the slot scheduler starts (round-robin mode): main yields
 * until the program's thread is dead.
 */
#include <elf.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <paging.h>
#include <pmm.h>
#include <sched.h>
#include <selftest.h>
#include <string.h>
#include <thread.h>
#include <user_test.h>
#include <usermode.h>
#include <vfs.h>

/* user/programs.asm: each program's ELF file, embedded byte for byte */
/* Run one program from the initramfs to completion; returns its exit code
   (-1 = didn't start). */
static int run(const char *path, const char *const argv[])
{
    struct thread *t = user_exec(path, argv);
    if (!t) {
        kprintf("User mode: %s failed to load\n", path);
        return -1;
    }
    while (t->state != THREAD_DEAD)
        thread_yield();
    user_cleanup(t);
    kprintf("User mode: %s exited with code %d\n", t->name, t->exit_code);
    return t->exit_code;
}

/* Plays "another program using the FPU": keeps wiping st0 and xmm0. If the
   kernel didn't save/restore user FPU state, the fpu program would see this
   thread's values instead of its own. */
static volatile int stop_wiping;

static void fpu_wiper(void *arg)
{
    (void)arg;
    while (!stop_wiping)
        __asm__ volatile ("fninit\n\t"            /* empty the x87 stack */
                          "fld1\n\t"              /* st0 = 1.0 */
                          "xorps %xmm0, %xmm0");   /* xmm0 = 0 */
}

/* Run the fpu program with preemption on, so it really gets switched out. */
static int run_fpu_test(void)
{
    static const char *const argv[] = { "fpu", NULL };
    stop_wiping = 0;
    struct thread *w = thread_create("wiper", fpu_wiper, NULL);
    sched_set_preempt(1);
    int code = run("/bin/fpu", argv);
    stop_wiping = 1;
    while (w && w->state != THREAD_DEAD)
        thread_yield();
    sched_set_preempt(0);
    return code;
}

/* Broken ELF files: each must be refused with -ENOEXEC. */
static void test_bad_elf(void)
{
    struct inode *hello;
    if (vfs_lookup("/bin/hello", &hello) < 0) {
        check(0, "elf: /bin/hello in the initramfs");
        return;
    }
    uint32_t size = hello->size;
    struct elf32_ehdr *copy = kmalloc(size);
    struct elf_info info;
    memcpy(copy, hello->data, size);

    check(elf_load("not an ELF file at all", 23, &info) == -ENOEXEC,
          "elf: garbage refused");
    check(elf_load(copy, 40, &info) == -ENOEXEC, "elf: truncated header refused");

    copy->e_machine = 62;                           /* x86-64 */
    check(elf_load(copy, size, &info) == -ENOEXEC, "elf: wrong CPU refused");
    copy->e_machine = EM_386;

    copy->e_phoff = 0xFFFFFFF0;                     /* headers past the end */
    check(elf_load(copy, size, &info) == -ENOEXEC, "elf: bad header offset refused");
    copy->e_phoff = ((const struct elf32_ehdr *)hello->data)->e_phoff;

    /* A segment that wants to load into the kernel's half of memory. */
    struct elf32_phdr *ph = (void *)((char *)copy + copy->e_phoff);
    ph[0].p_vaddr = 0xC0010000;
    check(elf_load(copy, size, &info) == -ENOEXEC, "elf: segment in kernel space refused");

    vmm_unmap_user_all();
    kfree(copy);
}

void user_selftest(void)
{
    static const char *const no_args[] = { NULL };
    static const char *const args[] = { "args", "one", "two", NULL };
    uint32_t free_before = pmm_free_count();

    static const char *const files_args[] = { "files", NULL };

    check(run("/bin/hello", no_args) == 42,
          "user: hello wrote, got -EFAULT for bad pointers, exit(42)");
    check(run("/bin/peek", no_args) == 139,
          "user: reading kernel memory kills the program, not the kernel");
    check(run("/bin/args", args) == 3,
          "user: argc/argv on the start-up stack");
    check(run("/bin/files", files_args) == 0,
          "user: open/read/close/stat/lseek on initramfs files (step 7)");
    check(run_fpu_test() == 0, "user: FPU/SSE registers survive thread switches");
    test_bad_elf();

    check(vmm_translate(0x08048000) == 0, "user: program memory unmapped");
    /* Every frame comes back, except the thread stacks (kmalloc'd, which
       may have grown the heap) — so compare against a little slack. */
    check(pmm_free_count() + 16 >= free_before, "user: program frames freed");
}
