/*
 * boottest — the system calls a real init and getty make while the machine
 * starts and stops (runit, agetty): sethostname, mount/umount2 (tmpfs, proc),
 * flock (runsv's supervise/lock), named pipes (supervise/control, ok), vhangup, and reboot's Ctrl-Alt-Del switch and magic-number check. The
 * reboot itself can't be tested from inside: it doesn't come back.
 * Also: users and groups (what login and doas do), and hundreds of memory areas, which every program linked against a
 * few dozen shared libraries has (musl's loader mprotects each one's RELRO).
 *
 * Prints "boottest: <check> ok" for each check and exits 0 if all pass.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <grp.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("boottest: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

/* The call failed, with this errno. */
static int fails(int ret, int err)
{
    return ret == -1 && errno == err;
}

static void hostname(void)
{
    struct utsname u;
    check(sethostname("livebox", 7) == 0 && uname(&u) == 0 &&
          strcmp(u.nodename, "livebox") == 0, "sethostname shows in uname");
    char longname[70];
    memset(longname, 'x', sizeof longname);
    check(fails(sethostname(longname, 65), EINVAL), "sethostname: 65 bytes is too long");
    check(sethostname("myos", 4) == 0 && uname(&u) == 0 &&
          strcmp(u.nodename, "myos") == 0, "sethostname back to myos");
}

/* PAM's pam_limits sets the nice value of every login session. */
static void priority(void)
{
    errno = 0;
    check(getpriority(PRIO_PROCESS, 0) == 0 && errno == 0, "getpriority: nice 0");
    check(setpriority(PRIO_PROCESS, 0, 5) == 0 && getpriority(PRIO_PROCESS, 0) == 5,
          "setpriority 5");
    check(setpriority(PRIO_PROCESS, 0, 0) == 0 && getpriority(PRIO_PROCESS, 0) == 0,
          "root may lower it again");
    check(fails(setpriority(PRIO_PROCESS, 99999, 0), ESRCH), "no such process: ESRCH");
}

static void tmpfs(void)
{
    const char *dir = "/boottest-run";
    struct stat st;
    struct statfs sf;
    mkdir(dir, 0755);
    check(mount("tmpfs", dir, "tmpfs", MS_NOSUID, "size=1m,mode=0700") == 0,
          "mount -t tmpfs");
    check(stat(dir, &st) == 0 && st.st_mode == (S_IFDIR | 0700), "tmpfs mode= option");
    check(statfs(dir, &sf) == 0 && sf.f_type == 0x01021994, "statfs says tmpfs");

    int fd = open("/boottest-run/f", O_CREAT | O_RDWR, 0644);
    char buf[8] = { 0 };
    check(fd >= 0 && write(fd, "hello", 5) == 5 && lseek(fd, 0, SEEK_SET) == 0 &&
          read(fd, buf, 5) == 5 &&
          strcmp(buf, "hello") == 0, "a file in the tmpfs");
    close(fd);
    check(unlink("/boottest-run/f") == 0, "remove it");

    check(fails(mount("tmpfs", "/bin/boottest", "tmpfs", 0, NULL), ENOTDIR),
          "mount on a file: ENOTDIR");
    check(fails(mount("x", "/boottest-none", "tmpfs", 0, NULL), ENOENT),
          "mount on a missing directory: ENOENT");
    check(fails(mount("sys", dir, "sysfs", 0, NULL), ENODEV), "sysfs: ENODEV");
    check(fails(mount("/dev/sda1", dir, "ext2", 0, NULL), ENOTBLK),
          "ext2 from a device: ENOTBLK");
    check(mount(NULL, dir, NULL, MS_REMOUNT | MS_NOSUID, NULL) == 0, "remount nosuid");
    check(fails(mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL), EBUSY),
          "remount / read-only: EBUSY");
    check(fails(mount(NULL, "/bin", NULL, MS_REMOUNT, NULL), EINVAL),
          "remount a plain directory: EINVAL");
    check(fails(umount2(dir, 0), EBUSY), "umount: EBUSY");
    check(fails(umount2("/bin", 0), EINVAL), "umount a plain directory: EINVAL");
}

/* /proc/self/fd: what ttyname() reads, so login knows its terminal. */
static void procfs(void)
{
    char link[32], buf[64];
    struct stat a, b;
    mkdir("/proc", 0555);
    check(mount("proc", "/proc", "proc", MS_NOSUID, NULL) == 0, "mount -t proc");

    int fd = open("/bin/boottest", O_RDONLY);
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, buf, sizeof buf - 1);
    check(n == 13 && memcmp(buf, "/bin/boottest", 13) == 0, "readlink /proc/self/fd/N");
    int again = open(link, O_RDONLY);
    check(again >= 0 && fstat(fd, &a) == 0 && fstat(again, &b) == 0 &&
          a.st_ino == b.st_ino && a.st_dev == b.st_dev, "open it: the same file");
    close(again);
    close(fd);
    check(fails(readlink(link, buf, sizeof buf), ENOENT), "a closed fd: ENOENT");

    int p[2];
    check(pipe(p) == 0 && (snprintf(link, sizeof link, "/proc/self/fd/%d", p[0]),
          n = readlink(link, buf, sizeof buf - 1)) > 6 && memcmp(buf, "pipe:[", 6) == 0,
          "a pipe: pipe:[N]");
    close(p[0]);
    close(p[1]);

    int tty = open("/dev/tty", O_RDWR);
    const char *name = tty >= 0 ? ttyname(tty) : NULL;
    check(name && strcmp(name, "/dev/tty") == 0, "ttyname");
    close(tty);

    check(chdir("/bin") == 0 && getcwd(buf, sizeof buf) && strcmp(buf, "/bin") == 0 &&
          chdir("/") == 0, "getcwd still right");
}

/* Two opens of one file are two lock owners; a fork shares its parent's. */
static void locks(void)
{
    const char *path = "/boottest-run/lock";
    int a = open(path, O_CREAT | O_RDWR, 0600);
    int b = open(path, O_RDWR);
    check(flock(a, LOCK_EX | LOCK_NB) == 0, "flock exclusive");
    check(fails(flock(b, LOCK_EX | LOCK_NB), EWOULDBLOCK), "a second one: EWOULDBLOCK");
    check(fails(flock(b, LOCK_SH | LOCK_NB), EWOULDBLOCK), "shared while exclusive: EWOULDBLOCK");
    pid_t pid = fork();
    if (pid == 0)
        _exit(flock(a, LOCK_EX | LOCK_NB) == 0 ? 0 : 1);   /* same open file */
    int st;
    check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "a forked child shares the lock");
    check(flock(a, LOCK_SH) == 0 && flock(b, LOCK_SH | LOCK_NB) == 0,
          "exclusive -> shared, then two shared");
    check(flock(a, LOCK_UN) == 0 && flock(b, LOCK_UN) == 0, "unlock");

    /* A child blocks on the lock until the parent's close drops it. */
    check(flock(a, LOCK_EX) == 0, "flock again");
    pid = fork();
    if (pid == 0) {
        close(a);                       /* its copy would keep the lock alive */
        close(b);
        int c = open(path, O_RDWR);
        _exit(flock(c, LOCK_EX) == 0 ? 0 : 1);
    }
    usleep(50000);
    close(a);                           /* the last fd: the lock goes */
    check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "close wakes a blocked flock");
    close(b);
    unlink(path);
}

static void fifos(void)
{
    const char *path = "/boottest-run/fifo";
    struct stat st;
    char buf[8] = { 0 };
    check(mkfifo(path, 0600) == 0 && stat(path, &st) == 0 && S_ISFIFO(st.st_mode),
          "mkfifo");
    check(fails(open(path, O_WRONLY | O_NONBLOCK), ENXIO), "write end, no reader: ENXIO");
    int r = open(path, O_RDONLY | O_NONBLOCK);
    int w = open(path, O_WRONLY | O_NONBLOCK);
    check(r >= 0 && w >= 0, "both ends, non-blocking");
    check(write(w, "ping", 4) == 4 && read(r, buf, sizeof buf) == 4 &&
          memcmp(buf, "ping", 4) == 0, "bytes go through");
    check(fails(read(r, buf, sizeof buf), EAGAIN), "empty: EAGAIN");
    check(fails(write(r, "x", 1), EBADF), "the read end can't write: EBADF");
    close(w);
    check(read(r, buf, sizeof buf) == 0, "writer gone: end of file");
    close(r);

    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path, O_WRONLY);  /* waits for the reader */
        _exit(fd >= 0 && write(fd, "hi", 2) == 2 ? 0 : 1);
    }
    usleep(50000);
    r = open(path, O_RDONLY);
    memset(buf, 0, sizeof buf);
    int n = read(r, buf, sizeof buf);
    int stc;
    check(n == 2 && memcmp(buf, "hi", 2) == 0 && waitpid(pid, &stc, 0) == pid &&
          WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "a blocking writer meets a reader");
    close(r);

    int rw = open(path, O_RDWR);        /* runsv keeps its pipes open this way */
    check(rw >= 0 && write(rw, "ok", 2) == 2 && read(rw, buf, 2) == 2, "O_RDWR never waits");
    close(rw);
    check(unlink(path) == 0, "remove the fifo");
}

static void hangup(void)
{
    signal(SIGHUP, SIG_IGN);            /* agetty does the same */
    check(vhangup() == 0, "vhangup");
    signal(SIGHUP, SIG_DFL);
}

static void reboot_switch(void)
{
    check(fails(syscall(SYS_reboot, 1, 2, RB_AUTOBOOT, 0), EINVAL), "reboot: bad magic");
    check(fails(syscall(SYS_reboot, 0xfee1dead, 672274793, 0x12345678, 0), EINVAL),
          "reboot: unknown command");
    check(reboot(RB_DISABLE_CAD) == 0, "Ctrl-Alt-Del off (to init)");
    check(reboot(RB_ENABLE_CAD) == 0, "Ctrl-Alt-Del on");
}

/* 600 pages, every other one read-only: 600 areas, each its own. */
static void many_areas(void)
{
    enum { N = 600, PG = 4096 };
    char *p = mmap(NULL, N * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(p != MAP_FAILED, "mmap 600 pages");
    int ok = 1;
    for (int i = 0; i < N; i += 2)
        ok &= mprotect(p + (i + 1) * PG, PG, PROT_READ) == 0;
    check(ok, "mprotect every other page: 600 areas");
    for (int i = 0; i < N; i += 2)
        p[i * PG] = (char)i;
    ok = 1;
    for (int i = 0; i < N; i++)
        ok &= p[i * PG] == (i % 2 ? 0 : (char)i);
    check(ok, "all of them read back");
    pid_t pid = fork();
    if (pid == 0) {
        int good = 1;
        for (int i = 0; i < N; i += 2)
            good &= p[i * PG] == (char)i;
        p[2 * PG] = 99;                 /* copy-on-write: the parent keeps its byte */
        _exit(good ? 0 : 1);
    }
    int st;
    check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
          p[2 * PG] == 2, "fork copies all 600");
    check(munmap(p, N * PG) == 0, "munmap them all at once");
}

/* What login does (setgroups, setgid, setuid), what a setuid program does
   (drop to the user, take root back through the saved id), and doas: exec
   of a setuid-root file. In a child, which ends as user 1000. */
static void users(void)
{
    check(getuid() == 0 && geteuid() == 0 && getgid() == 0, "we start as root");
    check(chmod("/bin/boottest", 04755) == 0, "make a setuid-root program");
    pid_t pid = fork();
    if (pid == 0) {
        gid_t g[2] = { 10, 20 }, got[4];
        int bad = 0;
        bad |= setgroups(2, g) != 0 || getgroups(4, got) != 2 || got[1] != 20;
        bad |= (setgid(100) != 0 || getgid() != 100 || getegid() != 100) << 1;
        bad |= (setresuid(1000, 1000, 0) != 0 || geteuid() != 1000) << 2;
        bad |= (!fails(setgroups(2, g), EPERM)) << 3;      /* euid isn't 0 now */
        bad |= (setuid(0) != 0 || geteuid() != 0 || getuid() != 1000) << 4;   /* saved id */
        bad |= (setuid(1000) != 0 || getuid() != 1000 || geteuid() != 1000) << 5;
        bad |= (!fails(setuid(0), EPERM)) << 6;            /* for good now */
        uid_t r, e, sv;
        bad |= (getresuid(&r, &e, &sv) != 0 || sv != 1000) << 7;
        struct stat st;
        int fd = open("/boottest-run/mine", O_CREAT | O_WRONLY, 0644);
        bad |= (fd < 0 || fstat(fd, &st) != 0 || st.st_uid != 1000 || st.st_gid != 100) << 8;
        close(fd);
        unlink("/boottest-run/mine");
        if (bad)
            _exit(bad & 0x7F ? bad & 0x7F : 0x80 | (bad >> 7));
        execl("/bin/boottest", "boottest", "--ids", (char *)NULL);
        _exit(0xFE);
    }
    int st;
    waitpid(pid, &st, 0);
    chmod("/bin/boottest", 0755);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code != 0)
        printf("boottest: (users: child said %d)\n", code);
    check(code == 0 || code == 0x7D || code == 0xFE,
          "setgroups/setgid/setuid, saved id, files owned by the user");
    check(code == 0, "exec of a setuid-root file: euid 0, AT_SECURE");
}

/* The strings on the start-up stack, in Linux's order: argv[0] first, the
   environment right after argv, one run (login rewrites it for `ps`). */
static void stack_strings(int argc, char **argv)
{
    extern char **environ;
    char *end = argv[argc - 1] + strlen(argv[argc - 1]) + 1;
    int ok = argv[0] < end && (!environ[0] || environ[0] == end);
    for (int i = 0; ok && environ[i] && environ[i + 1]; i++)
        ok = environ[i + 1] == environ[i] + strlen(environ[i]) + 1;
    check(ok, "argv and environment strings in Linux's order");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--ids") == 0)    /* run by users(), setuid */
        return getuid() == 1000 && geteuid() == 0 && getauxval(AT_SECURE) == 1 ? 0 : 0x7D;
    hostname();
    priority();
    tmpfs();
    procfs();
    locks();
    fifos();
    hangup();
    reboot_switch();
    many_areas();
    users();
    if (failures)
        return 1;
    printf("boottest: all ok\n");
    return 0;
}
