/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * systest.c — step 8 tests: what C libraries ask the kernel at startup
 *
 *   uname, clocks (realtime from the CMOS clock, monotonic), nanosleep,
 *   poll/select with timeouts, thread-local storage (set_thread_area + GS),
 *   getrandom, mprotect, openat with a directory fd, limits, mkdir.
 *
 * Exits 0 if all is well, else the number of the first failed check.
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("systest: check ");                         \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

#define SYS_mkdir 39
#define SYS_rmdir 40
#define SYS_uname 122
#define SYS_mprotect 125
#define PROT_NONE 0
#define SYS__newselect 142
#define SYS_nanosleep 162
#define SYS_poll 168
#define SYS_set_thread_area 243
#define SYS_openat 295
#define SYS_prlimit64 340
#define SYS_getrandom 355
#define SYS_clock_gettime64 403

static long long now_ms(int clock)
{
    long long ts[2];
    syscall3(SYS_clock_gettime64, clock, (long)ts, 0);
    return ts[0] * 1000 + ts[1] / 1000000;
}

static unsigned tls_block[4] = { 0xC0FFEE, 2, 3, 4 };

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int status, pid, p[2];

    /* 1. uname */
    static char u[6][65];
    CHECK(syscall3(SYS_uname, (long)u, 0, 0) == 0);
    CHECK(strcmp(u[0], "Linux") == 0 && strcmp(u[4], "i686") == 0);

    /* 2. Clocks: the date is after 2020; monotonic time moves with sleep. */
    CHECK(now_ms(0) / 1000 > 1577836800LL);
    long long t0 = now_ms(1);
    long req[2] = { 0, 30 * 1000000 };          /* 30 ms */
    CHECK(syscall3(SYS_nanosleep, (long)req, 0, 0) == 0);
    long long slept = now_ms(1) - t0;
    CHECK(slept >= 30 && slept < 200);

    /* 3. poll: an empty pipe times out; after a write it's readable. */
    CHECK(pipe(p) == 0);
    struct { int fd; short events, revents; } pf = { p[0], 1, 0 };
    t0 = now_ms(1);
    CHECK(syscall3(SYS_poll, (long)&pf, 1, 25) == 0);
    CHECK(now_ms(1) - t0 >= 25);
    write(p[1], "x", 1);
    CHECK(syscall3(SYS_poll, (long)&pf, 1, 1000) == 1 && (pf.revents & 1));

    /* 4. select sees the same. */
    unsigned rd = 1u << p[0];
    long tv[2] = { 1, 0 };
    CHECK(syscall5(SYS__newselect, p[0] + 1, (long)&rd, 0, 0, (long)tv) == 1);
    CHECK(rd == (1u << p[0]));
    close(p[0]);
    close(p[1]);

    /* 5. Thread-local storage: our own segment for GS, as musl does. */
    unsigned desc[4] = { (unsigned)-1, (unsigned)tls_block, 0xFFFFF, 0x51 };
    CHECK(syscall3(SYS_set_thread_area, (long)desc, 0, 0) == 0 && desc[0] == 6);
    unsigned sel = desc[0] * 8 + 3, v;
    __asm__ volatile ("mov %1, %%gs\n\tmov %%gs:0, %0" : "=r"(v) : "r"(sel));
    CHECK(v == 0xC0FFEE);
    pid = fork();                               /* the child keeps it */
    if (pid == 0) {
        __asm__ volatile ("mov %%gs:4, %0" : "=r"(v));
        _exit(v == 2 ? 0 : 1);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid && WEXITSTATUS(status) == 0);
    for (int i = 0; i < 5; i++)                 /* survives being switched out */
        sched_yield();
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(v));
    CHECK(v == 0xC0FFEE);

    /* 6. getrandom: different bytes each time. */
    unsigned r1[4], r2[4];
    CHECK(syscall3(SYS_getrandom, (long)r1, 16, 0) == 16);
    CHECK(syscall3(SYS_getrandom, (long)r2, 16, 0) == 16);
    CHECK(memcmp(r1, r2, 16) != 0);

    /* 7. mprotect: a read-only page can't be written (the child dies). */
    char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    m[0] = 'a';
    CHECK(syscall3(SYS_mprotect, (long)m, 4096, PROT_READ) == 0);
    pid = fork();
    if (pid == 0) {
        m[0] = 'b';
        _exit(0);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(m[0] == 'a');
    CHECK(syscall3(SYS_mprotect, (long)m, 4096, PROT_READ | PROT_WRITE) == 0);
    m[0] = 'c';
    CHECK(m[0] == 'c');

    /* 7b. PROT_NONE: not even a read. The page keeps its data, in this
       process and in a child forked meanwhile, for when it's readable again. */
    CHECK(syscall3(SYS_mprotect, (long)m, 4096, PROT_NONE) == 0);
    pid = fork();
    if (pid == 0) {
        volatile char c = m[0];
        (void)c;
        _exit(0);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    pid = fork();
    if (pid == 0)
        _exit(syscall3(SYS_mprotect, (long)m, 4096, PROT_READ) == 0 && m[0] == 'c' ? 0 : 1);
    CHECK(wait4(pid, &status, 0, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(syscall3(SYS_mprotect, (long)m, 4096, PROT_READ) == 0 && m[0] == 'c');
    /* A fresh PROT_NONE mapping (a guard page): its first touch faults,
       and the kernel won't write into it for us either. */
    char *g = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid = fork();
    if (pid == 0) {
        volatile char c = g[0];
        (void)c;
        _exit(0);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    int zp[2];
    CHECK(pipe(zp) == 0 && write(zp[1], "z", 1) == 1);
    CHECK(read(zp[0], g, 1) == -EFAULT);
    close(zp[0]);
    close(zp[1]);
    munmap(g, 4096);

    /* 8. openat relative to an open directory. */
    int dfd = open("/test", 0200000);           /* O_DIRECTORY */
    CHECK(dfd >= 0);
    int fd = (int)syscall5(SYS_openat, dfd, (long)"numbers.txt", 0, 0, 0);
    char b[2];
    CHECK(fd >= 0 && read(fd, b, 2) == 2 && b[0] == '1');
    close(fd);
    close(dfd);

    /* 9. Limits, and a directory made and removed (fstest does more). */
    unsigned long long lim[2];
    CHECK(syscall5(SYS_prlimit64, 0, 7, 0, (long)lim, 0) == 0 && lim[0] == 1024);
    CHECK(syscall3(SYS_mkdir, (long)"/newdir", 0755, 0) == 0);
    CHECK(syscall3(SYS_mkdir, (long)"/newdir", 0755, 0) == -EEXIST);
    CHECK(syscall3(SYS_rmdir, (long)"/newdir", 0, 0) == 0);

    put("systest: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
