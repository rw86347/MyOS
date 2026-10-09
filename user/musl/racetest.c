/*
 * racetest — attack-path tests for the one assumption this kernel was built
 * on and that turned out to be false: that a syscall is atomic because
 * interrupts are off for all of it.
 *
 * It is false because copy_to_user and copy_from_user can sleep. A fault on a
 * page of an mmap'd file goes to the page cache, which waits for the disk, and
 * another thread runs. Seven bugs came from code that read a shared index,
 * copied, and then wrote the index back (docs/security.md). Every fix for them
 * was an argument about an interleaving nobody had ever produced on purpose.
 *
 * These produce it. `syscall(423, 1)` arms the next copy by this thread to
 * yield before copying, which is what a sleep looks like from the sibling's
 * side (include/uaccess_test.h). Both exist only in `make test` builds.
 *
 * Each test asserts a conservation law rather than an implementation detail,
 * because that is what the bug violates and it stays true however the
 * scheduler happens to order things:
 *   - eight bytes written to a pipe can be read exactly once, in total;
 *   - two writers of eight bytes each put sixteen distinct bytes in;
 *   - one child can be reaped by exactly one waiter;
 *   - every name readdir returns is a name that is actually there.
 *
 * Prints "racetest: all N checks passed" when they hold.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NR_UACCESS_TEST 423

static int checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("racetest: FAIL %s\n", what);
    }
}

/* The sibling has to be runnable at the moment we yield, or the yield comes
   straight back to us and the interleaving never happens. It spins on a flag,
   and the timer preempts us so it gets to the spin in the first place. */
static volatile int sib_ready, sib_go, sib_done;

/* Arm the next copy by this thread to yield before copying — and to keep
   yielding until the sibling has done its part (sib_done), so the
   interleaving happens every time, not only when the scheduler happens to
   pick the sibling first. */
static int arm(void)
{
    return (int)syscall(NR_UACCESS_TEST, 1, &sib_done);
}

static void wait_for(volatile int *flag)
{
    for (int i = 0; i < 100000000 && !*flag; i++)
        sched_yield();
}

/* --- 1. two readers of one pipe ------------------------------------------- */

static int pipe_fd[2];
static volatile int b_read_ret, b_read_errno;
static char b_buf[64];

static void *reader(void *unused)
{
    (void)unused;
    sib_ready = 1;
    wait_for(&sib_go);
    b_read_ret = (int)read(pipe_fd[0], b_buf, 8);
    b_read_errno = errno;
    sib_done = 1;
    return 0;
}

static void test_pipe_read(void)
{
    if (pipe(pipe_fd) < 0) {
        check(0, "pipe: create");
        return;
    }
    /* Non-blocking, or the reader that loses the race waits forever for bytes
       the winner has already taken, and the test hangs instead of failing. */
    fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK);
    check(write(pipe_fd[1], "ABCDEFGH", 8) == 8, "pipe: 8 bytes written");

    sib_ready = sib_go = sib_done = 0;
    pthread_t t;
    pthread_create(&t, 0, reader, 0);
    wait_for(&sib_ready);

    char mine[64];
    sib_go = 1;
    check(arm() == 0, "pipe: the sleep could be armed (test build?)");
    int a = (int)read(pipe_fd[0], mine, 8);

    wait_for(&sib_done);
    pthread_join(t, 0);

    int b = b_read_ret;
    int got = (a > 0 ? a : 0) + (b > 0 ? b : 0);
    /* The whole point. With the index committed after the copy instead of
       before it, both readers measure the same 8 available bytes, both copy
       them, and both subtract -- so 16 bytes come out of an 8-byte write and
       the pipe's tail passes its head, after which `head - tail` is a number
       near four billion and nothing clamps the next read. */
    check(got == 8, "pipe: 8 bytes in, 8 bytes out in total");
    check(a == 8 || b == 8, "pipe: one reader got the whole write");
    check(!(a == 8 && b == 8), "pipe: the other reader did not get it as well");
    check(a > 0 || b > 0, "pipe: somebody got it");

    /* And the ring is still sane afterwards: a read of an empty non-blocking
       pipe says EAGAIN. If tail had passed head it would instead report
       billions of bytes available and copy kernel memory out. */
    errno = 0;
    int after = (int)read(pipe_fd[0], mine, sizeof mine);
    check(after < 0 && errno == EAGAIN, "pipe: empty afterwards, not underflowed");
    close(pipe_fd[0]);
    close(pipe_fd[1]);
}

/* --- 2. two writers of one pipe ------------------------------------------- */

static void *writer(void *unused)
{
    (void)unused;
    sib_ready = 1;
    wait_for(&sib_go);
    b_read_ret = (int)write(pipe_fd[1], "BBBBBBBB", 8);
    sib_done = 1;
    return 0;
}

static void test_pipe_write(void)
{
    if (pipe(pipe_fd) < 0) {
        check(0, "pipe write: create");
        return;
    }
    sib_ready = sib_go = sib_done = 0;
    pthread_t t;
    pthread_create(&t, 0, writer, 0);
    wait_for(&sib_ready);

    sib_go = 1;
    arm();
    int a = (int)write(pipe_fd[1], "AAAAAAAA", 8);

    wait_for(&sib_done);
    pthread_join(t, 0);
    check(a == 8 && b_read_ret == 8, "pipe write: both writers wrote 8");

    char out[64];
    int n = (int)read(pipe_fd[0], out, sizeof out);
    int as = 0, bs = 0;
    for (int i = 0; i < n; i++) {
        if (out[i] == 'A') as++;
        else if (out[i] == 'B') bs++;
    }
    /* Two writers that both claimed the same place in the ring would overwrite
       each other, so one letter would be short and the gap filled with
       whatever was there before. */
    check(n == 16, "pipe write: 16 bytes came out");
    check(as == 8 && bs == 8, "pipe write: neither writer overwrote the other");
    close(pipe_fd[0]);
    close(pipe_fd[1]);
}

/* --- 3. two waiters for one child ----------------------------------------- */

static volatile int b_wait_ret, b_wait_errno;

static void *waiter(void *unused)
{
    (void)unused;
    sib_ready = 1;
    wait_for(&sib_go);
    int st;
    errno = 0;
    b_wait_ret = (int)waitpid(-1, &st, WNOHANG);
    b_wait_errno = errno;
    sib_done = 1;
    return 0;
}

static void test_wait_reap(void)
{
    pid_t kid = fork();
    if (kid == 0)
        _exit(7);
    if (kid < 0) {
        check(0, "wait: fork");
        return;
    }
    /* Let it become a zombie before anyone looks. */
    for (int i = 0; i < 1000; i++)
        sched_yield();

    sib_ready = sib_go = sib_done = 0;
    b_wait_ret = -999;
    pthread_t t;
    pthread_create(&t, 0, waiter, 0);
    wait_for(&sib_ready);

    int st = 0;
    sib_go = 1;
    arm();                          /* the status copy is what sleeps */
    int a = (int)waitpid(-1, &st, 0);

    wait_for(&sib_done);
    pthread_join(t, 0);

    check(a == kid, "wait: the waiter that slept still got its child");
    check(WIFEXITED(st) && WEXITSTATUS(st) == 7, "wait: the status survived");
    /* Reaping after the copy instead of before it lets both threads see the
       same zombie and both return it. The slot is PROC_FREE by then, so a
       later fork reuses it and the second reap drops a live process's cwd. */
    check(b_wait_ret != kid, "wait: the sibling did not reap the same child");
    check(b_wait_ret <= 0, "wait: the sibling reaped nothing at all");
}

/* --- 4. readdir while the entries are being deleted ----------------------- */

#define RD_DIR "/tmp/racedir"
/*
 * A directory lists in the reverse of the order it was created in, because
 * sibling_insert puts a new child at the head of the list. So the walk below
 * sees "hhh" first and "aaa" last, and which entry survives decides what this
 * test is able to notice. Getting that backwards made it catch nothing, twice.
 *
 * "aaa" is the survivor, which puts it at the end of the walk:
 *   - everything the walk holds a pointer to *before* it has been reported is
 *     deleted, so a pointer held across a sleep without a reference really
 *     does dangle. If the first entry reported survived, that pointer would
 *     stay valid and a missing reference would be invisible;
 *   - and one entry is reported last and never deleted. POSIX leaves it
 *     unspecified whether an entry removed during a walk is reported, but an
 *     entry present for the whole walk must be -- and it is behind all the
 *     deletions, so a walk that loses its place loses it.
 */
static const char *rd_names[] = { "aaa", "bbb", "ccc", "ddd",
                                  "eee", "fff", "ggg", "hhh" };
#define RD_N ((int)(sizeof rd_names / sizeof rd_names[0]))
#define RD_KEPT 0                   /* created first, so reported last */

/* Names to groom the heap with. Three characters like the entries above, so
   the name allocation lands in the same kmalloc size class, and nothing a
   walk of RD_DIR could legitimately return. */
static const char *groom_names[] = {
    "Q01", "Q02", "Q03", "Q04", "Q05", "Q06", "Q07", "Q08",
    "Q09", "Q10", "Q11", "Q12", "Q13", "Q14", "Q15", "Q16",
};
#define RD_GROOM ((int)(sizeof groom_names / sizeof groom_names[0]))
#define RD_GROOM_DIR "/tmp/racegroom"

static void *unlinker(void *unused)
{
    (void)unused;
    sib_ready = 1;
    wait_for(&sib_go);
    /*
     * Everything except the survivor, so whichever entry the walk is holding
     * goes. With no reference taken, detach() calls inode_free on it the
     * moment refs reaches 0, which is immediately.
     *
     * The order is backwards for a reason, and it is the whole difference
     * between this noticing a dangling next_sibling and not. sibling_remove
     * leaves the removed node's own next_sibling pointer alone, so a detached
     * node still points at whatever followed it. Deleting in creation order
     * means deleting from the tail of the list towards the head, and by the
     * time each node is removed its next_sibling has already been updated to
     * the survivor -- every detached node ends up pointing at a live entry,
     * and a walk that follows one gets away with it.
     *
     * Deleting in list order instead (newest first) frees each node *after*
     * the one in front of it already pointed to it, so the detached nodes
     * point at freed memory, which is the situation the fix is for. An
     * attacker picks the order; the test should pick the bad one.
     */
    for (int i = RD_N - 1; i > RD_KEPT; i--) {
        char path[64];
        snprintf(path, sizeof path, "%s/%s", RD_DIR, rd_names[i]);
        unlink(path);
    }
    /* Then groom the heap. The inodes just freed went back on a kmalloc size
       class free list, and the next allocations of that size get them. Making
       files puts names of our choosing where a stale pointer in the walk is
       looking.
       Without this the test depends on luck: freed memory usually still holds
       the old name, so a walk reading a dangling pointer reads "aaa" back and
       sees nothing wrong. It is also what an exploit would do -- the point of
       a use-after-free is to choose what the freed object says. */
    for (int i = 0; i < RD_GROOM; i++) {
        char path[64];
        snprintf(path, sizeof path, "%s/%s", RD_GROOM_DIR, groom_names[i]);
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0)
            close(fd);
    }
    sib_done = 1;
    return 0;
}

static int known_name(const char *n)
{
    if (!strcmp(n, ".") || !strcmp(n, ".."))
        return 1;
    for (int i = 0; i < RD_N; i++)
        if (!strcmp(n, rd_names[i]))
            return 1;
    return 0;
}

static int make_entries(void)
{
    mkdir(RD_DIR, 0755);
    mkdir(RD_GROOM_DIR, 0755);
    for (int i = 0; i < RD_N; i++) {
        char path[64];
        snprintf(path, sizeof path, "%s/%s", RD_DIR, rd_names[i]);
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0)
            return -1;
        close(fd);
    }
    return 0;
}

static void drop_entries(void)
{
    char path[64];
    for (int i = 0; i < RD_N; i++) {
        snprintf(path, sizeof path, "%s/%s", RD_DIR, rd_names[i]);
        unlink(path);
    }
    for (int i = 0; i < RD_GROOM; i++) {
        snprintf(path, sizeof path, "%s/%s", RD_GROOM_DIR, groom_names[i]);
        unlink(path);
    }
    rmdir(RD_DIR);
    rmdir(RD_GROOM_DIR);
}

/*
 * Walk the directory with the nth copy of this thread armed to sleep, while a
 * sibling deletes the middle of it. Returns the number of things that went
 * wrong, and reports whether the surviving last entry was seen.
 *
 * The sweep over `nth` is the point. A single arming always lands on the copy
 * that emits ".", which is before the loop has an inode pointer of its own,
 * and two of the three bugs this is meant to catch live further in. Where the
 * sleep falls decides which pointer is stale, so the test has to try each.
 */
static int readdir_once(int nth, int *saw_survivor)
{
    int bad = 0;
    if (make_entries() < 0)
        return 1;

    int dfd = open(RD_DIR, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        drop_entries();
        return 1;
    }

    sib_ready = sib_go = sib_done = 0;
    pthread_t t;
    if (pthread_create(&t, 0, unlinker, 0) != 0) {
        close(dfd);
        drop_entries();
        return 1;
    }
    wait_for(&sib_ready);

    /* The size matters more than it looks. Every record here is 24 bytes, so
       this holds "." and ".." and three entries: enough that a sleep armed on
       an early copy is still inside the same call when the stale pointer gets
       used, and small enough that eight entries take several calls and the
       cursor has to be picked up again. At 64 bytes only "." and ".." fit, the
       call returns before the loop reads an entry at all, and the test quietly
       stops testing anything -- which it did, until verify-racetests said so. */
    char buf[128] __attribute__((aligned(8)));
    sib_go = 1;
    syscall(NR_UACCESS_TEST, nth);

    *saw_survivor = 0;
    /* The cap is a liveness check as much as a safety net: breaking out of the
       loop keeps the old cursor, and a walk that cannot get past a deleted
       entry would come back here for ever. */
    for (int call = 0; call < 200; call++) {
        int n = (int)syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (int off = 0; off + 19 <= n; ) {
            unsigned short reclen;
            memcpy(&reclen, buf + off + 16, 2);
            if (reclen < 19 || off + reclen > n) {
                bad++;                  /* a record length out of the blue */
                break;
            }
            const char *name = buf + off + 19;
            if (!known_name(name)) {
                /* A name read out of a freed inode, copied to user space. */
                bad++;
                printf("racetest:   nth=%d unexpected entry \"%.*s\"\n",
                       nth, (int)(reclen - 19), name);
            }
            if (!strcmp(name, rd_names[RD_KEPT]))
                *saw_survivor = 1;
            off += reclen;
        }
    }
    syscall(NR_UACCESS_TEST, 0);        /* in case the walk ended early */
    wait_for(&sib_done);
    pthread_join(t, 0);
    close(dfd);

    /* And the directory is still walkable afterwards: a cursor left anchored
       on a detached node would strand the entries behind it. */
    DIR *d = opendir(RD_DIR);
    int live = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                live++;
        closedir(d);
    }
    if (live != 1) {
        bad++;
        printf("racetest:   nth=%d %d entries left, expected 1\n", nth, live);
    }
    drop_entries();
    return bad;
}

static void test_readdir_unlink(void)
{
    int bad = 0, survivors = 0, sweep = 0;
    for (int nth = 1; nth <= 10; nth++) {
        int saw = 0;
        bad += readdir_once(nth, &saw);
        survivors += saw;
        sweep++;
    }
    /* Without a reference held across the copy, the entry being iterated is
       freed under us and its name pointer and length come out of the heap. */
    check(bad == 0, "readdir: every name returned was really there");
    /* An entry that exists for the whole walk has to be reported. It is the
       last one, behind every deletion, so a walk that follows a stale
       next_sibling out of the list never arrives at it. */
    check(survivors == sweep, "readdir: the entry that was never deleted was listed");
}

/*
 * Closing an fd while a sibling thread is blocked reading it.
 *
 * rw() in kernel/syscall.c fetches the struct file with fd_get and calls
 * f->ops->read(f, ...) without taking a reference. A blocking read sleeps
 * inside that call, so a sibling thread's close() can take the last
 * reference: file_put runs ->release and then kfree(f), and the sleeping
 * reader resumes holding a pointer into freed memory. clone(CLONE_THREAD)
 * puts the new thread in the same struct process, so the fd table is shared
 * outright and nothing stands in the way.
 *
 * pipe_read re-reads `f->flags` after every wake, so seeing it needs a wake
 * with nothing to read:
 *
 *   two readers sleep on one FIFO, one byte is written, both wake, one takes
 *   the byte and the other goes round the loop and tests f->flags again.
 *
 * The assertion is a rule rather than a value: read() on a blocking fd may
 * return data, or 0, or an error, but never EAGAIN. One that does has read
 * its O_NONBLOCK bit out of a struct file that was freed underneath it.
 *
 * FIFOs rather than pipe(): two opens of a FIFO give two struct files on one
 * pipe, so one can be freed while the other keeps the pipe alive and
 * writable. Closing an anonymous pipe's only read end takes the reader count
 * to zero with it, and then nothing can write to wake anybody.
 */
#define UAF_FIFO "/tmp/raceuaf"

static int uaf_rfd;                     /* the fd the blocked readers use */
static volatile int uaf_rc[2], uaf_errno[2];

static void *uaf_reader(void *arg)
{
    int i = (int)(long)arg;
    char c = 0;
    errno = 0;
    uaf_rc[i] = (int)read(uaf_rfd, &c, 1);
    uaf_errno[i] = errno;
    return 0;
}

static void spin(void)
{
    for (volatile int i = 0; i < 3000000; i++)
        ;
}

static void test_close_during_read(void)
{
    int eagain = 0, rounds = 0;

    for (int round = 0; round < 8; round++) {
        unlink(UAF_FIFO);
        if (mkfifo(UAF_FIFO, 0600) != 0)
            break;                      /* no FIFOs: nothing to test here */

        /* O_NONBLOCK so opening a reader does not wait for a writer; cleared
           again below, because the point is a read that blocks. */
        int r1 = open(UAF_FIFO, O_RDONLY | O_NONBLOCK);
        int r2 = open(UAF_FIFO, O_RDONLY | O_NONBLOCK);
        int w  = open(UAF_FIFO, O_WRONLY);
        if (r1 < 0 || r2 < 0 || w < 0) {
            if (r1 >= 0) close(r1);
            if (r2 >= 0) close(r2);
            if (w >= 0) close(w);
            break;
        }
        fcntl(r1, F_SETFL, 0);

        /* Both threads block on r1 -- the fd that is about to be closed, so
           the struct file they are asleep inside is the one that gets freed.
           r2 is the spare reference that keeps p->readers above zero, so the
           pipe stays writable and there is still something able to wake them.
           Blocking on r2 instead frees a file nobody is using, which is the
           mistake this comment exists to stop being made twice. */
        uaf_rfd = r1;
        uaf_rc[0] = uaf_rc[1] = -999;
        uaf_errno[0] = uaf_errno[1] = 0;
        pthread_t t[2];
        if (pthread_create(&t[0], 0, uaf_reader, (void *)0L) != 0 ||
            pthread_create(&t[1], 0, uaf_reader, (void *)1L) != 0) {
            close(r1); close(r2); close(w);
            break;
        }
        spin();                         /* let both reach the sleep */

        close(r1);

        /* Groom: the block just freed goes back on a kmalloc size class and
           the next allocation of that size gets it. Opening files puts a
           struct file there, with flags of our choosing. Freed memory that
           still holds its old contents hides the bug, which is the whole
           lesson of the readdir test above. */
        int g[8];
        for (int i = 0; i < 8; i++)
            g[i] = open("/etc/hostname", O_RDONLY | O_NONBLOCK);

        /* One byte, two sleepers: one leaves with it, the other goes round
           the loop and reads f->flags again. */
        write(w, "x", 1);
        spin();
        write(w, "y", 1);               /* let the second one out */

        pthread_join(t[0], 0);
        pthread_join(t[1], 0);
        for (int i = 0; i < 8; i++)
            if (g[i] >= 0)
                close(g[i]);
        close(r2);
        close(w);
        unlink(UAF_FIFO);

        rounds++;
        for (int i = 0; i < 2; i++)
            if (uaf_rc[i] < 0 && uaf_errno[i] == EAGAIN)
                eagain++;
    }

    if (rounds == 0) {
        printf("racetest: close-during-read needs FIFOs -- skipped\n");
        return;
    }
    check(eagain == 0, "close during read: a blocking read never returned EAGAIN");
}

/*
 * epoll_ctl while a sibling takes the epoll fd away.
 *
 * sys_epoll_ctl in fs/epoll.c fetched the struct epoll, then copied the
 * caller's epoll_event in, then wrote into the struct:
 *
 *      struct epoll *ep = epoll_of(a1, &err);     <- the object
 *      copy_from_user(&ev, (const void *)a4, ...) <- can sleep
 *      while (k < ep->n && ...)                   <- the object again
 *      ep->e[k].fd = ...; ep->n++;                <- written to
 *
 * The copy in the middle can sleep, and epoll_release does kfree(f->priv),
 * so a sibling can free `ep` while this thread is in the copy. It resumes
 * and writes into a freed heap block.
 *
 * dup2 rather than close, and that is the whole difficulty of writing this
 * test. With close() the next epoll_create1 takes back both the fd number
 * and the freed heap block, so "the kernel re-read the fd after the copy"
 * and "the kernel kept the pointer it had" land on the same object and
 * agree -- the first version of this test passed against the bug it was
 * written for, and verify-racetests.sh said so. dup2 frees the epoll the
 * same way (fd_install_at calls file_put on what was there) but leaves an
 * ordinary file at that fd number, so the two answers differ.
 *
 * The law: once that fd names something that is not an epoll, epoll_ctl on
 * it cannot succeed. Returning 0 means it added an entry to an interest list
 * that had already been freed.
 */
static int ep_fd = -1, ep_spare = -1;
static volatile int ep_rc, ep_errno;

static void *ep_taker(void *unused)
{
    (void)unused;
    sib_ready = 1;
    wait_for(&sib_go);
    dup2(ep_spare, ep_fd);          /* frees the epoll, keeps the fd number */
    sib_done = 1;
    return 0;
}

static void test_epoll_ctl_close(void)
{
    ep_fd = epoll_create1(0);
    ep_spare = open("/etc/hostname", O_RDONLY);
    int victim = open("/etc/hostname", O_RDONLY);
    if (ep_fd < 0 || ep_spare < 0 || victim < 0) {
        printf("racetest: epoll not available -- skipped\n");
        return;
    }

    sib_ready = sib_go = sib_done = 0;
    pthread_t t;
    if (pthread_create(&t, 0, ep_taker, 0) != 0) {
        close(ep_fd); close(ep_spare); close(victim);
        return;
    }
    wait_for(&sib_ready);

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = victim };
    sib_go = 1;
    arm();                          /* the next copy yields before copying */
    errno = 0;
    ep_rc = epoll_ctl(ep_fd, EPOLL_CTL_ADD, victim, &ev);
    ep_errno = errno;

    pthread_join(t, 0);
    close(ep_fd);
    close(ep_spare);
    close(victim);

    check(ep_rc < 0 && (ep_errno == EINVAL || ep_errno == EBADF),
          "epoll_ctl while the fd is replaced: no entry added to a freed list");
}

int main(void)
{
    if (arm() != 0) {
        printf("racetest: the test syscall is missing -- not a `make test` build\n");
        return 1;
    }
    syscall(NR_UACCESS_TEST, 0);        /* disarm what we just armed */

    test_pipe_read();
    test_pipe_write();
    test_wait_reap();
    test_readdir_unlink();
    test_close_during_read();
    test_epoll_ctl_close();

    if (failures) {
        printf("racetest: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("racetest: all %d checks passed\n", checks);
    return 0;
}
