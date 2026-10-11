/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pthreadtest — POSIX threads through musl's own pthread library, which is
 * what GLib (and so GTK, AbiWord...) is built on. Exercises clone with
 * CLONE_THREAD, futex, set_tid_address/pthread_join, thread-local storage,
 * per-thread signal masks, and exit vs. exit_group.
 *
 * Prints "pthreadtest: <check> ok" for each check and exits 0 if all pass.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("pthreadtest: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static pid_t tid(void)
{
    return syscall(SYS_gettid);
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* --- 1. a mutex under contention ------------------------------------------- */

#define WORKERS 4
#define ROUNDS  20000
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *add(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        long v = counter;
        if ((i & 1023) == 0)
            sched_yield();              /* hold the lock while others want it */
        counter = v + 1;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void test_mutex(void)
{
    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        pthread_create(&t[i], NULL, add, NULL);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(t[i], NULL);
    check(counter == (long)WORKERS * ROUNDS, "mutex");
}

/* --- 2. a condition variable (producer / consumer) ------------------------- */

static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;
static int queue[8], qhead, qtail;

static void *producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 1000; i++) {
        pthread_mutex_lock(&qlock);
        while (qtail - qhead == 8)
            pthread_cond_wait(&qcond, &qlock);
        queue[qtail++ % 8] = i;
        pthread_cond_broadcast(&qcond);
        pthread_mutex_unlock(&qlock);
    }
    return NULL;
}

static void test_cond(void)
{
    pthread_t t;
    pthread_create(&t, NULL, producer, NULL);
    long sum = 0;
    for (int n = 0; n < 1000; n++) {
        pthread_mutex_lock(&qlock);
        while (qtail == qhead)
            pthread_cond_wait(&qcond, &qlock);
        sum += queue[qhead++ % 8];
        pthread_cond_broadcast(&qcond);
        pthread_mutex_unlock(&qlock);
    }
    pthread_join(t, NULL);
    check(sum == 1000 * 1001 / 2, "condition variable");
}

/* --- 3. thread-local storage, tids, join values, detached threads ---------- */

static __thread int mine = 42;
static sem_t detached_done;

static void *tls_worker(void *arg)
{
    int n = (int)(long)arg;
    if (mine != 42)
        return (void *)-1L;
    mine = n;
    sched_yield();
    if (mine != n || getpid() == tid())
        return (void *)-1L;
    return (void *)(long)(n * 10);
}

static void *detached(void *arg)
{
    (void)arg;
    sem_post(&detached_done);
    return NULL;
}

static void test_tls(void)
{
    pthread_t t[3];
    for (int i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, tls_worker, (void *)(long)(i + 1));
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        void *r;
        pthread_join(t[i], &r);
        ok &= (long)r == (i + 1) * 10;
    }
    check(ok && mine == 42 && getpid() == tid(), "thread-local storage and join");

    sem_init(&detached_done, 0, 0);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < 20; i++) {      /* their slots must be recycled */
        pthread_t d;
        if (pthread_create(&d, &a, detached, NULL) != 0) {
            check(0, "detached threads (create)");
            return;
        }
        sem_wait(&detached_done);
    }
    check(1, "detached threads");
}

/* --- 4. signals: to one thread, and to the process -------------------------- */

static volatile pid_t handled_by;
static volatile pid_t worker_tid;
static volatile int go;

static void on_usr1(int sig)
{
    (void)sig;
    handled_by = tid();
}

static void *wait_for_signal(void *arg)
{
    if (arg) {                          /* undo the mask inherited from main */
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    }
    worker_tid = tid();
    go = 1;
    while (!handled_by)
        sched_yield();
    return NULL;
}

static void test_signals(void)
{
    signal(SIGUSR1, on_usr1);

    pthread_t t;
    handled_by = 0; go = 0;
    pthread_create(&t, NULL, wait_for_signal, NULL);
    while (!go)
        sched_yield();
    pthread_kill(t, SIGUSR1);
    pthread_join(t, NULL);
    check(handled_by == worker_tid, "pthread_kill reaches that thread");

    /* kill() is for the whole process: with main blocking SIGUSR1, only the
       worker may take it. */
    sigset_t s, old;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &s, &old);
    handled_by = 0; go = 0;
    pthread_create(&t, NULL, wait_for_signal, (void *)1);
    while (!go)
        sched_yield();
    kill(getpid(), SIGUSR1);
    pthread_join(t, NULL);
    check(handled_by == worker_tid, "kill picks a thread that doesn't block it");
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

/* --- 5. timeouts ------------------------------------------------------------- */

static void test_timeout(void)
{
    sem_t s;
    sem_init(&s, 0, 0);
    struct timespec t0, until;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 50 * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    int r = sem_timedwait(&s, &until);
    int err = errno;
    long ms = ms_since(&t0);
    check(r == -1 && err == ETIMEDOUT && ms >= 45 && ms < 1000, "sem_timedwait times out");
}

/* --- 7. POSIX timers: timer_create & co., sigtimedwait ------------------------ */

#ifndef SIGEV_THREAD_ID
#define SIGEV_THREAD_ID 4
#endif

static volatile int notified, notified_value;

static void on_timer(union sigval v)
{
    notified_value = v.sival_int;
    notified = 1;
}

static volatile int ticks_seen, ticks_ok;

static void on_tick(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)uc;
    ticks_seen++;
    if (si->si_code == SI_TIMER && si->si_value.sival_int == 42)
        ticks_ok++;
}

/* A timer that signals this thread only (so no other thread can take it). */
static int timer_for_me(clockid_t clk, int sig, int value, timer_t *id)
{
    struct sigevent ev;
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = sig;
    ev.sigev_value.sival_int = value;
    ev.sigev_notify_thread_id = tid();
    return timer_create(clk, &ev, id);
}

static void test_timers(void)
{
    struct timespec t0;
    timer_t id;

    /* Exactly what Vim's start_timeout() does ('redrawtime', regexp
       timeouts): SIGEV_THREAD on CLOCK_MONOTONIC. musl runs the function
       on a helper thread that waits in sigwaitinfo for an SI_TIMER signal. */
    struct sigevent ev;
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_THREAD;
    ev.sigev_notify_function = on_timer;
    ev.sigev_value.sival_int = 7;
    int made = timer_create(CLOCK_MONOTONIC, &ev, &id) == 0;
    struct itimerspec it = { { 0, 0 }, { 0, 20 * 1000000 } };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int set = made && timer_settime(id, 0, &it, NULL) == 0;
    while (set && !notified && ms_since(&t0) < 2000)
        usleep(1000);
    long ms = ms_since(&t0);
    check(made && set && notified && notified_value == 7 && ms >= 20,
          "SIGEV_THREAD timer calls its function after 20 ms (Vim's timeout)");
    if (made)
        timer_delete(id);

    /* Every 5 ms, to this thread, with SI_TIMER and our si_value. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_tick;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN, &sa, NULL);
    made = timer_for_me(CLOCK_REALTIME, SIGRTMIN, 42, &id) == 0;
    it = (struct itimerspec){ { 0, 5000000 }, { 0, 5000000 } };
    struct itimerspec cur = { { 0, 0 }, { 0, 0 } };
    set = made && timer_settime(id, 0, &it, NULL) == 0 && timer_gettime(id, &cur) == 0;
    check(set && cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 5000000 &&
          cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 &&
          cur.it_value.tv_nsec <= 5000000,
          "timer_gettime: the interval, and at most the time that was set");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (set && ticks_seen < 4 && ms_since(&t0) < 2000)
        usleep(1000);
    check(ticks_seen >= 4 && ticks_ok == ticks_seen,
          "periodic timer: signal with SI_TIMER and si_value, every time");
    int deleted = made && timer_delete(id) == 0;
    int seen = ticks_seen;
    usleep(20000);
    check(deleted && ticks_seen == seen, "timer_delete stops it");
    /* (musl's timer_delete returns the raw -EINVAL; errno stays as it was) */
    int again = timer_delete(id);
    check(again == -EINVAL || (again == -1 && errno == EINVAL),
          "timer_delete of a deleted timer: EINVAL");

    /* sigtimedwait: a timeout, then a blocked timer signal taken with its
       siginfo — from an absolute CLOCK_REALTIME time already past. */
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGRTMIN + 1);
    pthread_sigmask(SIG_BLOCK, &wait_set, NULL);
    siginfo_t si;
    struct timespec to = { 0, 10 * 1000000 };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = sigtimedwait(&wait_set, &si, &to);
    check(r == -1 && errno == EAGAIN && ms_since(&t0) >= 10, "sigtimedwait times out with EAGAIN");

    made = timer_for_me(CLOCK_REALTIME, SIGRTMIN + 1, 99, &id) == 0;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    it = (struct itimerspec){ { 0, 0 }, { now.tv_sec - 1, 0 } };
    set = made && timer_settime(id, TIMER_ABSTIME, &it, NULL) == 0;
    memset(&si, 0, sizeof si);
    to = (struct timespec){ 2, 0 };
    r = set ? sigtimedwait(&wait_set, &si, &to) : -1;
    check(r == SIGRTMIN + 1 && si.si_code == SI_TIMER && si.si_value.sival_int == 99 &&
          si.si_timerid == (int)(intptr_t)id,
          "TIMER_ABSTIME in the past fires at once; sigwaitinfo gets its siginfo");

    /* Every 1 ms while the signal stays blocked: one signal, the rest counted. */
    it = (struct itimerspec){ { 0, 1000000 }, { 0, 1000000 } };
    set = made && timer_settime(id, 0, &it, NULL) == 0;
    usleep(30000);
    memset(&si, 0, sizeof si);
    r = set ? sigtimedwait(&wait_set, &si, &to) : -1;
    int over = made ? timer_getoverrun(id) : -1;
    check(r == SIGRTMIN + 1 && si.si_overrun >= 10 && over >= 10,
          "a blocked periodic timer counts overruns");

    /* Deleting it takes back its pending signal: unblocking must not
       deliver it (by default, a real-time signal ends the program). */
    usleep(5000);
    sigset_t pend;
    sigpending(&pend);
    int was = sigismember(&pend, SIGRTMIN + 1);
    if (made)
        timer_delete(id);
    sigpending(&pend);
    check(was && !sigismember(&pend, SIGRTMIN + 1),
          "timer_delete takes back its pending signal");
    pthread_sigmask(SIG_UNBLOCK, &wait_set, NULL);
}

/* --- 6. processes with threads: fork, exit, pthread_exit, exec ---------------- */

static int child_status(pid_t pid)
{
    int st;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void *sleeper(void *arg)
{
    (void)arg;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&m);
    for (;;)
        pthread_cond_wait(&c, &m);      /* (a futex wait nobody wakes) */
    return NULL;
}

static void *spinner(void *arg)
{
    (void)arg;
    for (volatile int forever = 1; forever; )
        ;
    return NULL;
}

static void *exit3(void *arg)
{
    (void)arg;
    exit(3);
}

static int pipefd[2];

static void *outlive_main(void *arg)
{
    (void)arg;
    usleep(20000);
    char c = 'x';
    write(pipefd[1], &c, 1);
    return (void *)9;                   /* last thread out: the process exits 0 */
}

static void test_processes(void)
{
    pthread_t t;
    pthread_create(&t, NULL, sleeper, NULL);

    /* fork: the child is a copy of the calling thread only */
    pid_t pid = fork();
    if (pid == 0)
        _exit(getpid() == tid() ? 7 : 1);
    check(child_status(pid) == 7, "fork in a threaded process");

    /* exit() ends every thread, including one asleep in a futex */
    pid = fork();
    if (pid == 0) {
        pthread_t a, b;
        pthread_create(&a, NULL, sleeper, NULL);
        pthread_create(&b, NULL, spinner, NULL);
        usleep(10000);
        exit(4);
    }
    check(child_status(pid) == 4, "exit() ends all threads");

    /* ...also when a thread other than main calls it */
    pid = fork();
    if (pid == 0) {
        pthread_t a;
        pthread_create(&a, NULL, exit3, NULL);
        pthread_join(a, NULL);          /* never returns */
        _exit(1);
    }
    check(child_status(pid) == 3, "exit() from a second thread");

    /* main may end first; the process lives on with its other thread */
    pipe(pipefd);
    pid = fork();
    if (pid == 0) {
        pthread_t a;
        pthread_create(&a, NULL, outlive_main, NULL);
        pthread_exit(NULL);
    }
    char c = 0;
    read(pipefd[0], &c, 1);
    check(c == 'x' && child_status(pid) == 0, "main thread exits first");

    /* execve keeps only the calling thread */
    pid = fork();
    if (pid == 0) {
        pthread_t a, b;
        pthread_create(&a, NULL, sleeper, NULL);
        pthread_create(&b, NULL, spinner, NULL);
        char *argv[] = { "true", NULL };
        execve("/bin/true", argv, NULL);
        _exit(1);
    }
    check(child_status(pid) == 0, "execve from a threaded process");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_mutex();
    test_cond();
    test_tls();
    test_signals();
    test_timeout();
    test_timers();
    test_processes();
    printf("pthreadtest: %s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;           /* (exit_group with "sleeper" asleep) */
}
