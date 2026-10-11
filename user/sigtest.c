/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * sigtest.c — step 8 tests: signals
 *
 *   handlers    kill(self) runs the handler before kill() returns; plain
 *               and SA_SIGINFO handlers; registers and FPU survive it
 *   masks       a blocked signal waits (sigpending), runs when unblocked
 *   interrupt   a child asleep in read() is woken: -EINTR with a handler,
 *               killed by the default action (SIGTERM) without one
 *   SIGCHLD     a child's exit calls the parent's handler
 *   SIGPIPE     kills a writer to a closed pipe, by default
 *   sigsuspend  waits for a signal with a temporary mask
 *   groups      kill(-pgid) reaches every process in a group
 *
 * Exits 0 if all is well, else the number of the first failed check.
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("sigtest: check ");                         \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

static volatile int got, got_sig, info_signo, chld;

static void on_usr1(int sig)
{
    got++;
    got_sig = sig;
    volatile double x = 1.5;            /* the FPU in a handler */
    x = x * 3.0;
    (void)x;
}

static void on_info(int sig, struct siginfo *info, void *uc)
{
    (void)sig; (void)uc;
    info_signo = info->si_signo;
}

static void on_chld(int sig)
{
    (void)sig;
    chld++;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int me = getpid(), p[2], pid, status;
    char c;

    /* 1. A handler runs before kill() returns; registers and FPU intact. */
    CHECK(sigaction_simple(SIGUSR1, on_usr1, 0, 0) == 0);
    volatile double keep = 2.25;
    int local = 1234;
    CHECK(kill(me, SIGUSR1) == 0);
    CHECK(got == 1 && got_sig == SIGUSR1);
    CHECK(local == 1234 && keep == 2.25);

    /* 2. SA_SIGINFO: the handler gets a siginfo. */
    CHECK(sigaction_simple(SIGUSR2, (void (*)(int))(void (*)(void))on_info, SA_SIGINFO, 0) == 0);
    CHECK(kill(me, SIGUSR2) == 0 && info_signo == SIGUSR2);

    /* 3. Blocked: it waits; unblocked: it runs at once. */
    sigset_t set = SIGBIT(SIGUSR1), pend = 0;
    CHECK(sigprocmask(SIG_BLOCK, &set, 0) == 0);
    kill(me, SIGUSR1);
    CHECK(got == 1);
    CHECK(sigpending(&pend) == 0 && (pend & SIGBIT(SIGUSR1)));
    CHECK(sigprocmask(SIG_UNBLOCK, &set, 0) == 0 && got == 2);

    /* 4. A child asleep in read() with a handler: read returns -EINTR. */
    CHECK(pipe(p) == 0);
    pid = fork();
    if (pid == 0) {
        close(p[1]);
        _exit(read(p[0], &c, 1) == -EINTR && got == 3 ? 7 : 1);
    }
    for (int i = 0; i < 10; i++)        /* let it run until it sleeps in read() */
        sched_yield();
    CHECK(kill(pid, SIGUSR1) == 0);
    CHECK(wait4(pid, &status, 0, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7);

    /* 5. ...and without a handler, SIGTERM's default action ends it. */
    pid = fork();
    if (pid == 0) {
        close(p[1]);
        read(p[0], &c, 1);
        _exit(0);
    }
    for (int i = 0; i < 10; i++)
        sched_yield();
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(wait4(pid, &status, 0, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    close(p[0]);
    close(p[1]);

    /* 6. SIGCHLD: the parent hears about its child's exit. */
    CHECK(sigaction_simple(SIGCHLD, on_chld, 0, 0) == 0);
    pid = fork();
    if (pid == 0)
        _exit(0);
    CHECK(wait4(pid, &status, 0, 0) == pid);
    CHECK(chld == 1);

    /* 7. SIGPIPE's default action ends a writer to a closed pipe. */
    pid = fork();
    if (pid == 0) {
        int q[2];
        pipe(q);
        close(q[0]);
        write(q[1], "x", 1);
        _exit(0);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);

    /* 8. sigsuspend: block USR1, a child sends it, suspend lets it in. */
    CHECK(sigprocmask(SIG_BLOCK, &set, 0) == 0);
    pid = fork();
    if (pid == 0) {
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    sigset_t none = 0;
    int before = got;
    CHECK(sigsuspend(&none) == -EINTR && got == before + 1);
    sigset_t now = 0;
    CHECK(sigprocmask(SIG_BLOCK, 0, &now) == 0 && (now & SIGBIT(SIGUSR1)));   /* restored */
    CHECK(sigprocmask(SIG_UNBLOCK, &set, 0) == 0);
    wait4(pid, &status, 0, 0);

    /* 9. Process groups: two children in a new group, one kill for both. */
    int pg = 0;
    for (int i = 0; i < 2; i++) {
        pid = fork();
        if (pid == 0) {
            setpgid(0, pg ? pg : 0);
            for (;;)                    /* busy: SIGTERM must still get it */
                ;
        }
        setpgid(pid, pg ? pg : pid);
        if (!pg)
            pg = pid;
    }
    CHECK(kill(-pg, SIGTERM) == 0);
    for (int i = 0; i < 2; i++)
        CHECK(wait4(-1, &status, 0, 0) > 0 && WTERMSIG(status) == SIGTERM);

    put("sigtest: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
