/*
 * forktest.c — step 8 tests: processes and memory
 *
 *   fork        the child gets a copy-on-write copy: its writes don't show
 *               in the parent, and both see their own pid / parent pid
 *   wait4       exit statuses, -ECHILD, WNOHANG, a child killed by a fault
 *   many        several children alive at once, all reaped
 *   brk, mmap   heap and anonymous memory, pages created on first touch
 *   stack       deep recursion: the stack grows on demand
 *   execve      fork + exec of another program, and of a #! script
 *
 * Exits 0 if all is well, else the number of the first failed check.
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("forktest: check ");                        \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

static int shared_value = 1;            /* in the data segment */

/* Uses ~200 KiB of stack: more than the start-up stack pages. */
static int deep(int n)
{
    volatile char pad[1000];
    pad[0] = (char)n;
    return n == 0 ? pad[0] : deep(n - 1) + 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int status, pid;
    int me = getpid();

    /* 1. fork + copy-on-write + exit status. */
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {                     /* the child */
        shared_value = 2;               /* a private copy now */
        _exit(getppid() == me ? 40 + shared_value : 1);
    }
    CHECK(pid != me);
    CHECK(wait4(pid, &status, 0, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    CHECK(shared_value == 1);           /* the child's write stayed private */

    /* 2. No more children: -ECHILD. WNOHANG with a running child: 0. */
    CHECK(wait4(-1, &status, 0, 0) == -ECHILD);

    /* 3. Several children at once, reaped in any order. */
    int sum = 0;
    for (int i = 1; i <= 5; i++)
        if (fork() == 0)
            _exit(i);
    for (int i = 0; i < 5; i++) {
        CHECK(wait4(-1, &status, 0, 0) > 0);
        sum += WEXITSTATUS(status);
    }
    CHECK(sum == 15);

    /* 4. A child killed by a page fault: SIGSEGV (11). */
    pid = fork();
    if (pid == 0) {
        *(volatile int *)0 = 1;         /* NULL pointer */
        _exit(0);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == 11);

    /* 5. brk: grow the heap by 1 MiB and use all of it. */
    char *start = sys_brk(0);
    char *end = sys_brk(start + (1 << 20));
    CHECK(end == start + (1 << 20));
    for (int i = 0; i < (1 << 20); i += 4096)
        start[i] = (char)i;
    CHECK(start[4096 * 7] == (char)(4096 * 7));
    CHECK(sys_brk(start) == start);     /* give it back */

    /* 6. mmap / munmap: 2 MiB of anonymous memory. */
    char *m = mmap(0, 2 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK((long)m > 0);
    m[0] = 'a';
    m[(2 << 20) - 1] = 'z';
    CHECK(m[0] == 'a' && m[(2 << 20) - 1] == 'z');
    CHECK(munmap(m, 2 << 20) == 0);

    /* 7. The stack grows on demand. */
    CHECK(deep(200) == 200);

    /* 8. fork + execve: the child becomes /bin/args. */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "args", "from", "exec", 0 };
        char *env[] = { "X=1", 0 };
        execve("/bin/args", args, env);
        _exit(99);                      /* only if execve failed */
    }
    CHECK(wait4(pid, &status, 0, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3);

    /* 9. execve of a missing file fails and returns. */
    char *none[] = { "nope", 0 };
    CHECK(execve("/bin/nope", none, none) == -ENOENT);

    /* 10. A #! script: runs its interpreter with the script as argument. */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/script.sh", "x", 0 };
        execve("/bin/script.sh", args, none);
        _exit(99);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid);
    /* args prints its argv and exits with argc: "/bin/args -v /bin/script.sh x" = 4 */
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 4);

    put("forktest: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
