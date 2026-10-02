/*
 * bench.c — user-side benchmarks: what does a system call cost?
 *
 * getpid() is the classic "null" system call: it does almost nothing in
 * the kernel, so its time is the price of entering and leaving the kernel.
 * Then the file operations from step 7. We time many calls with the cycle
 * counter and print the average.
 */
#include <mini.h>

#define N 20000

/* Same layout as the kernel's lines: name padded to 28, number to 8. */
static void report_n(const char *what, uint64_t cycles, uint32_t n)
{
    char num[21];
    int i = sizeof num;
    uint64_t v = cycles / n;
    num[--i] = '\0';
    do {
        num[--i] = '0' + (char)(v % 10);
        v /= 10;
    } while (v);

    put("  ");
    put(what);
    for (size_t n = strlen(what); n < 29; n++)
        put(" ");
    for (int pad = 8 - (int)(sizeof num - 1 - i); pad > 0; pad--)
        put(" ");
    put(&num[i]);
    put(" cycles\n");
}

static void report(const char *what, uint64_t cycles)
{
    report_n(what, cycles, N);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t t;

    for (int i = 0; i < 100; i++)               /* warm up the caches */
        sys_int80(SYS_getpid, 0, 0, 0);
    t = rdtsc();
    for (int i = 0; i < N; i++)
        sys_int80(SYS_getpid, 0, 0, 0);
    report("null syscall, int 0x80", rdtsc() - t);

    if (__vsyscall) {
        for (int i = 0; i < 100; i++)
            sys_vsys(SYS_getpid, 0, 0, 0);
        t = rdtsc();
        for (int i = 0; i < N; i++)
            sys_vsys(SYS_getpid, 0, 0, 0);
        report("null syscall, sysenter", rdtsc() - t);
    } else {
        put("  null syscall, sysenter       (kernel offers no AT_SYSINFO)\n");
    }

    /* Files (initramfs): path lookup, reading, stat. */
    static char buf[4096];
    struct statx st;
    t = rdtsc();
    for (int i = 0; i < N; i++)
        close(open("/test/numbers.txt", O_RDONLY));
    report("open + close (2-part path)", rdtsc() - t);

    int fd = open("/test/numbers.txt", O_RDONLY);
    t = rdtsc();
    for (int i = 0; i < N; i++) {
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof buf);
    }
    report("lseek + read 4 KiB", rdtsc() - t);
    close(fd);

    t = rdtsc();
    for (int i = 0; i < N; i++)
        statx(AT_FDCWD, "/test/numbers.txt", 0, STATX_BASIC_STATS, &st);
    report("stat (statx)", rdtsc() - t);

    /* Processes (step 8): fork is copy-on-write, so it copies page tables,
       not memory. */
    enum { P = 500 };
    t = rdtsc();
    for (int i = 0; i < P; i++) {
        int pid = fork();
        if (pid == 0)
            _exit(0);
        wait4(pid, 0, 0, 0);
    }
    report_n("fork + exit + wait", rdtsc() - t, P);

    char *targv[] = { "true", 0 }, *tenv[] = { 0 };
    t = rdtsc();
    for (int i = 0; i < P; i++) {
        int pid = fork();
        if (pid == 0) {
            execve("/bin/true", targv, tenv);
            _exit(1);
        }
        wait4(pid, 0, 0, 0);
    }
    report_n("fork + exec + exit + wait", rdtsc() - t, P);
    return 0;
}
