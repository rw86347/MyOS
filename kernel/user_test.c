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
#include <bcache.h>
#include <pcache.h>
#include <elf.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <linux/fb.h>
#include <paging.h>
#include <pmm.h>
#include <sched.h>
#include <selftest.h>
#include <string.h>
#include <thread.h>
#include <user_test.h>
#include <proc.h>
#include <timer.h>
#include <tty.h>
#include <vfs.h>

/* user/programs.asm: each program's ELF file, embedded byte for byte */
/* Run one program from the initramfs to completion; returns its exit code,
   or 128 + the signal that killed it (like a shell shows it), or -1 if it
   didn't start. */
static int run(const char *path, const char *const argv[])
{
    static const char *const envp[] = { "HOME=/", "TEST=1", NULL };
    struct process *p = proc_spawn(path, argv, envp);
    if (!p) {
        kprintf("User mode: %s failed to load\n", path);
        return -1;
    }
    /* The name comes from `path` and not from p->threads->name, which is the
       whole point: proc_spawn makes the program runnable before it returns,
       so one that exits immediately can have its thread freed and unlinked
       from p->threads before this line runs. p->threads is NULL then and the
       read comes from address 0 -- a kernel page fault that took out about
       one test run in ten, and only ever after a short program.
       Reading it through the thread cannot be made safe by a NULL check
       either: the thread can go between the check and the copy. `path` is the
       caller's string literal, it outlives the wait, and proc_spawn derives
       the thread's name from it the same way (base_name in kernel/proc.c). */
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            name = s + 1;
    int status = proc_wait_kernel(p);
    int code = W_IFEXITED(status) ? W_EXITSTATUS(status) : 128 + W_TERMSIG(status);
    kprintf("User mode: %s exited with code %d\n", name, code);
    return code;
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

/* Run /bin/ttytest and "type" at it: each time it waits in read(), the
   next keys go in (as if from the keyboard). */
static int run_tty_test(void)
{
    static const char *const argv[] = { "ttytest", NULL };
    static const char *const envp[] = { NULL };
    static const char *const keys[] = {
        "hello wq\x7forld\r",          /* Backspace is DEL (0x7F) */
        "\x04",                         /* Ctrl-D */
        "\033[A",                       /* up arrow, in raw mode */
        "\x03",                         /* Ctrl-C */
    };
    struct process *p = proc_spawn("/bin/ttytest", argv, envp);
    if (!p)
        return -1;
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        uint32_t t0 = timer_ms();
        while (!tty_reader_waiting() && p->state == PROC_ALIVE && timer_ms() - t0 < 2000)
            thread_yield();
        tty_input(keys[i], strlen(keys[i]));
    }
    int status = proc_wait_kernel(p);
    int code = W_IFEXITED(status) ? W_EXITSTATUS(status) : 128 + W_TERMSIG(status);
    kprintf("User mode: ttytest exited with code %d\n", code);
    tty_set_foreground(0);
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

    check(elf_load("not an ELF file at all", 23, ELF_DYN_BASE, &info) == -ENOEXEC,
          "elf: garbage refused");
    check(elf_load(copy, 40, ELF_DYN_BASE, &info) == -ENOEXEC, "elf: truncated header refused");

    copy->e_machine = 62;                           /* x86-64 */
    check(elf_load(copy, size, ELF_DYN_BASE, &info) == -ENOEXEC, "elf: wrong CPU refused");
    copy->e_machine = EM_386;

    copy->e_phoff = 0xFFFFFFF0;                     /* headers past the end */
    check(elf_load(copy, size, ELF_DYN_BASE, &info) == -ENOEXEC, "elf: bad header offset refused");
    copy->e_phoff = ((const struct elf32_ehdr *)hello->data)->e_phoff;

    /* A segment that wants to load into the kernel's half of memory. */
    struct elf32_phdr *ph = (void *)((char *)copy + copy->e_phoff);
    ph[0].p_vaddr = 0xC0010000;
    check(elf_load(copy, size, ELF_DYN_BASE, &info) == -ENOEXEC, "elf: segment in kernel space refused");

    kfree(copy);
}

void user_selftest(void)
{
    static const char *const no_args[] = { NULL };
    static const char *const args[] = { "args", "one", "two", NULL };
    uint32_t free_before = pmm_free_count(), heap_before = kheap_pages();
    uint32_t cache_before = bcache_pages(), pc_before = pcache_pages();
    sched_set_preempt(1);               /* programs share the CPU, as for real */

    static const char *const files_args[] = { "files", NULL };

    check(run("/bin/hello", no_args) == 42,
          "user: hello wrote, got -EFAULT for bad pointers, exit(42)");
    check(run("/bin/peek", no_args) == 139,
          "user: reading kernel memory kills the program, not the kernel");
    check(run("/bin/args", args) == 3,
          "user: argc/argv on the start-up stack");
    check(run("/bin/files", files_args) == 0,
          "user: open/read/close/stat/lseek on initramfs files (step 7)");
    static const char *const fork_args[] = { "forktest", NULL };
    check(run("/bin/forktest", fork_args) == 0,
          "user: fork, wait4, brk, mmap, stack growth, execve (step 8)");
    static const char *const pipe_args[] = { "pipetest", NULL };
    check(run("/bin/pipetest", pipe_args) == 0,
          "user: pipes, dup2, fcntl, getdents64, chdir, symlinks (step 8)");
    static const char *const sig_args[] = { "sigtest", NULL };
    check(run("/bin/sigtest", sig_args) == 0,
          "user: signals: handlers, masks, EINTR, SIGCHLD, groups (step 8)");
    static const char *const sys_args[] = { "systest", NULL };
    check(run("/bin/systest", sys_args) == 0,
          "user: uname, clocks, sleep, poll, TLS, mprotect, openat (step 8)");
    static const char *const fs_args[] = { "fstest", NULL };
    check(run("/bin/fstest", fs_args) == 0,
          "user: writable files: create, write, rename, rm -r, exec (step 8i)");
    static const char *const ext2_args[] = { "ext2test", NULL };
    check(run("/bin/ext2test", ext2_args) == 0,
          "user: ext2: files made by Linux's mke2fs, mount rules, persist.txt (step 9b)");
    static const char *const fs_disk_args[] = { "fstest", "/data/fst", NULL };
    check(run("/bin/fstest", fs_disk_args) == 0,
          "user: all of fstest again, on the ext2 disk (step 9b)");
    static const char *const pthread_args[] = { "pthreadtest", NULL };
    check(run("/bin/pthreadtest", pthread_args) == 0,
          "user: musl pthreads: mutex, condvar, TLS, join, signals, exit/exec (threads)");
    /* Deliberately interleaves two threads inside a copy_to_user, which is
       the one thing seven of this round's bugs needed and no test produced
       (docs/security.md). Test builds only: it needs the arming syscall. */
    static const char *const race_args[] = { "racetest", NULL };
    check(run("/bin/racetest", race_args) == 0,
          "user: a copy to user space that sleeps cannot be raced (pipe, wait4, readdir)");
    static const char *const shm_args[] = { "shmtest", NULL };
    check(run("/bin/shmtest", shm_args) == 0,
          "user: System V shared memory: shmget/shmat/shmdt/shmctl, fork, IPC_RMID");
    static const char *const boot_args[] = { "boottest", NULL };
    check(run("/bin/boottest", boot_args) == 0,
          "user: init's calls: sethostname, mount tmpfs, umount2, vhangup, reboot switches");
    check(run_tty_test() == 0,
          "user: terminal: line editing, EOF, raw mode, Ctrl-C (step 8)");
    if (fb_get(0)) {                    /* only with a display card (QEMU's std VGA) */
        static const char *const fb_args[] = { "fbtest", NULL };
        check(run("/bin/fbtest", fb_args) == 0,
              "user: /dev/fb0: screen info, mmap, fork, pan, mode change, KD_GRAPHICS (step 11b)");
    }
    check(run_fpu_test() == 0, "user: FPU/SSE registers survive thread switches");
    sched_set_preempt(0);
    test_bad_elf();

    check(vmm_translate(0x08048000) == 0, "user: program memory unmapped");
    /* Every page comes back — exactly. The only pages allowed to stay
       missing are the ones the kernel heap took to grow, and the two
       caches (both keep them). */
    check(pmm_free_count() + (kheap_pages() - heap_before) +
          (bcache_pages() - cache_before) + (pcache_pages() - pc_before) == free_before,
          "user: every program page freed (none leaked)");
}
