/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fstest.c — the writable file system (step 8i)
 *
 *   create, write, append, read back, holes, truncate (shrink and grow),
 *   rename (all the replace rules), symlinks, deleting an open file,
 *   rmdir rules, `rm -r`-style deleting while reading a directory,
 *   chmod and exec permission, running a script and a program that were
 *   written at run time, utimensat, mknod, statfs.
 *
 * Exits 0 if all is well, else the number of the first failed check.
 * Everything it makes is deleted again (the kernel's leak check counts
 * every page).
 */
#include <mini.h>

static int checks;

#define CHECK(cond)                                         \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            put("fstest ");                                 \
            put(base);                                      \
            put(": check ");                                \
            put_u64(checks);                                \
            put(" failed: " #cond "\n");                    \
            return checks;                                  \
        }                                                   \
    } while (0)

#define SYS_unlink     10
#define SYS_mknod      14
#define SYS_chmod      15
#define SYS_rename     38
#define SYS_mkdir      39
#define SYS_rmdir      40
#define SYS_symlink    83
#define SYS_ftruncate  93
#define SYS_truncate64 193
#define SYS_statfs64   268
#define SYS_unlinkat   301
#define SYS_utimensat  320

static int creat_mode(const char *path, int flags, int mode)
{
    return (int)syscall3(SYS_open, (long)path, flags, mode);
}

static long call2(long nr, const char *a, const char *b)
{
    return syscall3(nr, (long)a, (long)b, 0);
}

static int stat_of(const char *path, struct statx *st)
{
    return statx(AT_FDCWD, path, 0x100 /* don't follow */, STATX_BASIC_STATS, st);
}

static long size_of(const char *path)
{
    struct statx st;
    return stat_of(path, &st) < 0 ? -1 : (long)st.stx_size;
}

/* Write a whole buffer to a new file. */
static int put_file(const char *path, const void *data, size_t n, int mode)
{
    int fd = creat_mode(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0)
        return fd;
    long w = write(fd, data, n);
    close(fd);
    return w == (long)n ? 0 : -1;
}

/* Run a program and return its exit code (or -errno if exec failed). */
static int run(const char *path)
{
    int pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)path, 0 };
        char *envp[] = { 0 };
        long err = execve(path, argv, envp);
        _exit(200 + (int)-err);         /* report the exec error */
    }
    int status;
    if (wait4(pid, &status, 0, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static char buf[12000];

/* Where to work: argv[1] (default /fst, on the RAM file system). Run with
   /data/fst it tests the disk file system the same way. */
static const char *base = "/fst";

/* base + "/" + name, in one of 4 rotating buffers (a call may need two). */
static const char *P(const char *name)
{
    static char bufs[4][128];
    static int next;
    char *b = bufs[next++ & 3];
    int n = 0;
    for (const char *s = base; *s; s++)
        b[n++] = *s;
    if (*name) {
        b[n++] = '/';
        for (const char *s = name; *s; s++)
            b[n++] = *s;
    }
    b[n] = '\0';
    return b;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        base = argv[1];
    char b[64];
    struct statx st;
    int fd;

    /* 1. A directory and a new file in it. */
    CHECK(call2(SYS_mkdir, P(""), (const char *)0755) == 0);
    CHECK(call2(SYS_mkdir, P(""), (const char *)0755) == -EEXIST);
    fd = creat_mode(P("a"), O_CREAT | O_EXCL | O_WRONLY, 0666);
    CHECK(fd >= 3);
    CHECK(write(fd, "hello", 5) == 5);
    CHECK(read(fd, b, 1) == -EBADF);            /* opened write-only */
    close(fd);
    CHECK(creat_mode(P("a"), O_CREAT | O_EXCL | O_WRONLY, 0666) == -EEXIST);
    CHECK(stat_of(P("a"), &st) == 0 && st.stx_size == 5);
    CHECK((st.stx_mode & 07777) == 0644);       /* 0666 minus the umask 022 */
    CHECK(stat_of(P(""), &st) == 0 && (st.stx_mode & S_IFMT) == S_IFDIR);

    /* 2. Read it back; append. */
    fd = open(P("a"), O_RDONLY);
    CHECK(fd >= 3 && read(fd, b, 64) == 5 && memcmp(b, "hello", 5) == 0);
    CHECK(write(fd, "x", 1) == -EBADF);         /* opened read-only */
    close(fd);
    fd = open(P("a"), O_WRONLY | O_APPEND);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, " world", 6) == 6);         /* O_APPEND: at the end anyway */
    close(fd);
    CHECK(size_of(P("a")) == 11);

    /* 3. A file across several pages, and a hole after it. */
    for (int i = 0; i < (int)sizeof buf; i++)
        buf[i] = (char)(i % 251);
    CHECK(put_file(P("big"), buf, sizeof buf, 0644) == 0);
    fd = open(P("big"), O_RDWR);
    CHECK(lseek(fd, 20000, SEEK_SET) == 20000 && write(fd, "Z", 1) == 1);
    CHECK(size_of(P("big")) == 20001);
    static char back[12000];
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, back, sizeof back) == sizeof back);
    CHECK(memcmp(buf, back, sizeof buf) == 0);
    CHECK(lseek(fd, 16000, SEEK_SET) == 16000 && read(fd, b, 8) == 8);
    int zeros = 1;
    for (int i = 0; i < 8; i++)
        zeros &= b[i] == 0;
    CHECK(zeros);                               /* the hole reads as zeros */

    /* 4. Truncate: shrink, then grow again — the cut-off bytes must not
          come back. */
    CHECK(syscall3(SYS_ftruncate, fd, 3, 0) == 0 && size_of(P("big")) == 3);
    CHECK(syscall3(SYS_ftruncate, fd, 5000, 0) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, back, 5000) == 5000);
    zeros = back[0] == 0 && back[1] == 1 && back[2] == 2;
    for (int i = 3; i < 5000; i++)
        zeros &= back[i] == 0;
    CHECK(zeros);
    close(fd);
    CHECK(syscall3(SYS_truncate64, (long)P("big"), 100, 0) == 0);
    CHECK(size_of(P("big")) == 100);
    fd = open(P("big"), O_WRONLY | O_TRUNC);  /* O_TRUNC empties it */
    close(fd);
    CHECK(size_of(P("big")) == 0);

    /* 5. rename, and its rules for what may replace what. */
    CHECK(call2(SYS_rename, P("a"), P("b")) == 0);
    CHECK(open(P("a"), O_RDONLY) == -ENOENT && size_of(P("b")) == 11);
    CHECK(call2(SYS_rename, P("b"), P("big")) == 0);   /* replaces big */
    CHECK(size_of(P("big")) == 11 && size_of(P("b")) == -1);
    CHECK(call2(SYS_mkdir, P("d"), (const char *)0755) == 0);
    CHECK(call2(SYS_rename, P("d"), P("big")) == -ENOTDIR);
    CHECK(call2(SYS_rename, P("big"), P("d")) == -EISDIR);
    CHECK(call2(SYS_rename, P("d"), P("d/inside")) == -EINVAL);
    CHECK(call2(SYS_rename, P("big"), P("d/f")) == 0);  /* into a directory */
    CHECK(call2(SYS_rename, P("d"), P("e")) == 0);      /* a directory moves */
    CHECK(size_of(P("e/f")) == 11);

    /* 6. Symlinks. */
    CHECK(call2(SYS_symlink, "e/f", P("link")) == 0);
    CHECK(readlink(P("link"), b, sizeof b) == 3 && memcmp(b, "e/f", 3) == 0);
    fd = open(P("link"), O_RDONLY);
    CHECK(fd >= 3 && read(fd, b, 5) == 5 && memcmp(b, "hello", 5) == 0);
    close(fd);
    CHECK(call2(SYS_symlink, "nowhere", P("dangling")) == 0);
    CHECK(open(P("dangling"), O_RDONLY) == -ENOENT);
    CHECK(stat_of(P("dangling"), &st) == 0 && (st.stx_mode & S_IFMT) == S_IFLNK);

    /* 7. Deleting a file that's still open: it lives on until closed. */
    fd = open(P("e/f"), O_RDONLY);
    CHECK(call2(SYS_unlink, P("e/f"), 0) == 0);
    CHECK(open(P("e/f"), O_RDONLY) == -ENOENT);
    CHECK(read(fd, b, 5) == 5 && memcmp(b, "hello", 5) == 0);
    CHECK(statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &st) == 0 && st.stx_nlink == 0);
    close(fd);

    /* 8. rmdir and unlink rules. */
    CHECK(put_file(P("e/g"), "1", 1, 0644) == 0);
    CHECK(call2(SYS_rmdir, P("e"), 0) == -ENOTEMPTY);
    CHECK(call2(SYS_unlink, P("e"), 0) == -EISDIR);
    CHECK(call2(SYS_rmdir, P("e/g"), 0) == -ENOTDIR);
    CHECK(call2(SYS_unlink, P("e/g"), 0) == 0);
    CHECK(call2(SYS_rmdir, P("e/"), 0) == 0);             /* trailing slash */
    CHECK(call2(SYS_rmdir, "/", 0) < 0);

    /* 9. `rm -r`: read a directory a few entries at a time, deleting each
          entry right after it's returned. Every file must come up exactly
          once, and the directory must end up empty. */
    CHECK(call2(SYS_mkdir, P("many"), (const char *)0755) == 0);
    char name[] = "many/f00";
    for (int i = 0; i < 40; i++) {
        name[6] = (char)('0' + i / 10);
        name[7] = (char)('0' + i % 10);
        CHECK(put_file(P(name), "", 0, 0644) == 0);
    }
    int dfd = open(P("many"), O_DIRECTORY);
    int seen[40] = { 0 }, total = 0, n;
    char dbuf[100];                             /* about 3 entries per call */
    while ((n = getdents64(dfd, dbuf, sizeof dbuf)) > 0) {
        for (int off = 0; off < n; ) {
            unsigned short reclen;
            memcpy(&reclen, dbuf + off + 16, 2);
            const char *nm = dbuf + off + 19;
            if (nm[0] == 'f') {
                int k = (nm[1] - '0') * 10 + (nm[2] - '0');
                seen[k]++;
                total++;
                CHECK(syscall3(SYS_unlinkat, dfd, (long)nm, 0) == 0);
            }
            off += reclen;
        }
    }
    CHECK(n == 0 && total == 40);
    int once = 1;
    for (int i = 0; i < 40; i++)
        once &= seen[i] == 1;
    CHECK(once);
    close(dfd);
    CHECK(call2(SYS_rmdir, P("many"), 0) == 0);

    /* 10. Exec permission, and running files written at run time: a
           script, and a copy of a program. */
    const char script[] = "#!/bin/sh\nexit 7\n";
    CHECK(put_file(P("s.sh"), script, sizeof script - 1, 0644) == 0);
    CHECK(run(P("s.sh")) == 200 + EACCES);    /* no x bit */
    CHECK(call2(SYS_chmod, P("s.sh"), (const char *)0755) == 0);
    CHECK(run(P("s.sh")) == 7);
    fd = open("/bin/hello", O_RDONLY);
    int out = creat_mode(P("hello"), O_CREAT | O_WRONLY, 0755);
    while ((n = read(fd, buf, sizeof buf)) > 0)
        CHECK(write(out, buf, n) == n);
    close(fd);
    close(out);
    CHECK(run(P("hello")) == 42);

    /* 11. Times, a device node, and how full the file system is. */
    int times[4] = { 1000, 0, 2000, 0 };        /* access, modify (seconds, ns) */
    CHECK(syscall5(SYS_utimensat, AT_FDCWD, (long)P("hello"), (long)times, 0, 0) == 0);
    CHECK(stat_of(P("hello"), &st) == 0 && st.stx_atime.tv_sec == 1000 &&
          st.stx_mtime.tv_sec == 2000);
    CHECK(syscall3(SYS_mknod, (long)P("null"), S_IFCHR | 0666, (1 << 8) | 3) == 0);
    fd = open(P("null"), O_WRONLY);
    CHECK(fd >= 3 && write(fd, "gone", 4) == 4);    /* /dev/null's driver */
    close(fd);
    unsigned fs[21];
    CHECK(syscall3(SYS_statfs64, (long)P(""), 84, (long)fs) == 0);
    CHECK((fs[0] == 0x01021994 && fs[1] == 4096) ||     /* RAM: tmpfs, 4 KiB blocks */
          (fs[0] == 0xEF53 && fs[1] >= 1024));          /* a disk: ext2 */

    /* 12. Clean up: nothing may be left behind. */
    const char *const files[] = { "link", "dangling", "s.sh", "hello", "null" };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++)
        CHECK(call2(SYS_unlink, P(files[i]), 0) == 0);
    CHECK(call2(SYS_rmdir, P(""), 0) == 0);

    put("fstest ");
    put(base);
    put(": all ");
    put_u64(checks);
    put(" checks passed\n");
    return 0;
}
