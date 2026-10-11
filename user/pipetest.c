/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * pipetest.c — step 8 tests: pipes, fds, directories, links
 *
 *   pipe        parent and child talk through a pipe; 100 KB through a
 *               4 KiB buffer (both sides must sleep and wake), EOF, EPIPE
 *   dup2+exec   a child's stdout becomes the pipe, then it execs /bin/args:
 *               the parent reads args' output — that's `args | cat`
 *   fcntl       non-blocking reads, close-on-exec flag
 *   getdents64  listing "/", chdir/getcwd, relative paths
 *   symlinks    /bin/argslink -> args: readlink, exec through the link
 *   writev      two buffers in one write; /dev/null swallows writes
 *
 * Exits 0 if all is well, else the number of the first failed check.
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("pipetest: check ");                        \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

/* Read until end of file; returns the number of bytes. */
static long read_all(int fd, char *buf, long max)
{
    long total = 0, n;
    while (total < max && (n = read(fd, buf + total, max - total)) > 0)
        total += n;
    return total;
}

/* Does the directory listing in buf (getdents64 format) contain name? */
static int has_entry(const char *buf, int len, const char *name)
{
    for (int at = 0; at < len; ) {
        unsigned short reclen = *(const unsigned short *)(buf + at + 16);
        if (strcmp(buf + at + 19, name) == 0)
            return 1;
        at += reclen;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char buf[4096];
    int p[2], status, pid;

    /* 1. A message through a pipe; end of file when the writer closes. */
    CHECK(pipe(p) == 0 && p[0] >= 3 && p[1] >= 3);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "hello pipe", 10);
        _exit(0);
    }
    close(p[1]);
    CHECK(read_all(p[0], buf, sizeof buf) == 10 && memcmp(buf, "hello pipe", 10) == 0);
    close(p[0]);
    CHECK(wait4(pid, &status, 0, 0) == pid);

    /* 2. 100 KB through the 4 KiB pipe: writer and reader take turns
          sleeping. Every byte must arrive, in order. */
    CHECK(pipe(p) == 0);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        static char out[1000];
        for (int i = 0; i < 100; i++) {
            for (int j = 0; j < 1000; j++)
                out[j] = (char)(i + j);
            if (write(p[1], out, 1000) != 1000)
                _exit(1);
        }
        _exit(0);
    }
    close(p[1]);
    long total = 0, n;
    int ok = 1;
    while ((n = read(p[0], buf, 777)) > 0) {    /* odd size on purpose */
        for (long k = 0; k < n; k++) {
            long at = total + k;
            if (buf[k] != (char)(at / 1000 + at % 1000))
                ok = 0;
        }
        total += n;
    }
    CHECK(total == 100000 && ok);
    close(p[0]);
    CHECK(wait4(pid, &status, 0, 0) == pid && WEXITSTATUS(status) == 0);

    /* 3. `args | cat`: the child's stdout is the pipe, then it becomes
          /bin/args, whose output we read. */
    CHECK(pipe(p) == 0);
    pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);                  /* stdout -> the pipe */
        close(p[0]);
        close(p[1]);
        char *a[] = { "args", "through", "a", "pipe", 0 }, *e[] = { 0 };
        execve("/bin/args", a, e);
        _exit(99);
    }
    close(p[1]);
    n = read_all(p[0], buf, sizeof buf);
    CHECK(n == 21 && memcmp(buf, "args through a pipe \n", 21) == 0);
    close(p[0]);
    CHECK(wait4(pid, &status, 0, 0) == pid && WEXITSTATUS(status) == 4);

    /* 4. Writing when nobody can read: SIGPIPE (ignored here) and -EPIPE. */
    CHECK(sigaction_simple(SIGPIPE, SIG_IGN, 0, 0) == 0);
    CHECK(pipe(p) == 0);
    close(p[0]);
    CHECK(write(p[1], "x", 1) == -EPIPE);
    close(p[1]);

    /* 5. Non-blocking read of an empty pipe: -EAGAIN. Close-on-exec flag. */
    CHECK(pipe(p) == 0);
    CHECK(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
    CHECK(read(p[0], buf, 1) == -EAGAIN);
    int d = fcntl(p[0], F_DUPFD_CLOEXEC, 10);
    CHECK(d >= 10 && fcntl(d, F_GETFD, 0) == 1);
    close(d);
    close(p[0]);
    close(p[1]);

    /* 6. Directories: list "/", chdir, getcwd, relative open. */
    int fd = open("/", O_RDONLY);
    CHECK(fd >= 0);
    n = getdents64(fd, buf, sizeof buf);
    CHECK(n > 0 && has_entry(buf, n, "bin") && has_entry(buf, n, "dev") &&
          has_entry(buf, n, "README.txt") && has_entry(buf, n, ".."));
    CHECK(getdents64(fd, buf, sizeof buf) == 0);     /* that was all */
    close(fd);
    CHECK(chdir("/test") == 0);
    CHECK(getcwd(buf, sizeof buf) == 6 && strcmp(buf, "/test") == 0);
    fd = open("numbers.txt", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, 2) == 2 && memcmp(buf, "1\n", 2) == 0);
    close(fd);
    CHECK(chdir("..") == 0 && getcwd(buf, sizeof buf) == 2 && strcmp(buf, "/") == 0);

    /* 7. Symbolic links: readlink, and exec through one. */
    CHECK(readlink("/bin/argslink", buf, sizeof buf) == 4 && memcmp(buf, "args", 4) == 0);
    pid = fork();
    if (pid == 0) {
        char *a[] = { "argslink", 0 }, *e[] = { 0 };
        execve("/bin/argslink", a, e);
        _exit(99);
    }
    CHECK(wait4(pid, &status, 0, 0) == pid && WEXITSTATUS(status) == 1);

    /* 8. writev, and /dev/null. */
    struct iovec iov[2] = { { "pipetest: writev ", 17 }, { "works\n", 6 } };
    CHECK(writev(1, iov, 2) == 23);
    fd = open("/dev/null", O_WRONLY);
    CHECK(fd >= 0 && write(fd, "gone", 4) == 4 && read(fd, buf, 4) == 0);
    close(fd);

    put("pipetest: all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
