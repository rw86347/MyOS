/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * diskbench.c — CPU work per disk operation, the same binary on MyOS and Linux
 *
 * Under QEMU's -icount the clock jumps ahead while the CPU idles (waiting for
 * the disk), so timing a read() says nothing. Instead a child process spins
 * on the cycle counter and adds up the gaps where something else had the
 * CPU: our reads, the kernel, the disk interrupts. Minus the gaps of an idle
 * stretch (timer ticks), that is the CPU work the I/O cost.
 *
 * Phases tell the spinner what is being measured: the parent writes the
 * phase number into a pipe and sleeps a moment so the spinner sees it
 * before the work starts. A gap is booked to the phase the spinner was in
 * when it saw the gap, which is the phase the work belonged to.
 *
 * argv[1]: directory with `big` and `big2` (8 MiB each) on the disk under
 * test. Linux as /init: it loads /modules.list, mounts the disk on /mnt.
 */
#include <mini.h>

#define SYS_unlink      10
#define SYS_mount       21
#define SYS_reboot      88
#define SYS_fsync      118
#define SYS_nanosleep  162
#define SYS_finit_module 350

#define CHUNK   65536
#define FILE_SZ (8u << 20)
#define NPHASE  6
#define CHECK   1024            /* spinner loop passes between pipe checks */
#define GAP     150

static const char *phase_name[NPHASE] = {
    "idle (baseline)",
    "cold read 64 KiB, sequential",
    "warm read 64 KiB, sequential",
    "warm read 4 KiB, random",
    "cold read 4 KiB, random",
    "write 64 KiB (+ fsync at end)",
};
static const uint32_t phase_ops[NPHASE] = { 1, FILE_SZ / CHUNK, FILE_SZ / CHUNK, 2048, 512, 64 };

static uint8_t buf[CHUNK] __attribute__((aligned(4096)));

static void msleep(long ms)
{
    long ts[2] = { ms / 1000, (ms % 1000) * 1000000 };
    syscall3(SYS_nanosleep, (long)ts, 0, 0);
}

static void put_line(const char *what, uint64_t v, const char *unit)
{
    put("diskbench: ");
    put(what);
    for (size_t n = strlen(what); n < 34; n++)
        put(" ");
    put_u64(v);
    put(unit);
    put("\n");
}

/* --- the spinner ------------------------------------------------------------ */

struct result { uint64_t gaps[NPHASE], elapsed[NPHASE]; };

static void spinner(int cmd, int out)
{
    struct result r;
    memset(&r, 0, sizeof r);
    int phase = -1;                 /* -1: not started */
    uint64_t prev = rdtsc(), start = prev;
    for (uint32_t n = 0;; n++) {
        uint64_t now = rdtsc();
        if (now - prev > GAP && phase >= 0)
            r.gaps[phase] += now - prev;
        prev = now;
        if (n % CHECK)
            continue;
        uint8_t p;
        long k = read(cmd, &p, 1);          /* non-blocking */
        uint64_t after = rdtsc();
        prev = after;                       /* our own syscall is not a gap */
        if (k != 1)
            continue;
        if (phase >= 0)
            r.elapsed[phase] += now - start;
        start = after;
        if (p == 0xFF)
            break;
        phase = p;
    }
    write(out, &r, sizeof r);
    _exit(0);
}

/* --- the work ------------------------------------------------------------- */

static int cmdw;

static void begin(int phase)
{
    uint8_t p = (uint8_t)phase;
    write(cmdw, &p, 1);
    msleep(20);                     /* let the spinner switch */
}

static int open3(const char *path, int flags, int mode)
{
    return (int)syscall3(SYS_open, (long)path, flags, mode);
}

static char path_big[64], path_big2[64], path_w[64];

static void join(char *dst, const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    memcpy(dst, dir, a);
    dst[a] = '/';
    memcpy(dst + a + 1, name, b + 1);
}

static int work(void)
{
    begin(0);
    msleep(200);                    /* idle: only the timer ticks */

    int fd = open(path_big, O_RDONLY);
    if (fd < 0)
        return -1;
    begin(1);
    for (uint32_t i = 0; i < FILE_SZ / CHUNK; i++)
        if (read(fd, buf, CHUNK) != CHUNK)
            return -2;
    begin(2);
    lseek(fd, 0, SEEK_SET);
    for (uint32_t i = 0; i < FILE_SZ / CHUNK; i++)
        if (read(fd, buf, CHUNK) != CHUNK)
            return -3;
    begin(3);
    for (uint32_t i = 0; i < 2048; i++) {
        lseek(fd, (long)((i * 7919u) % (FILE_SZ / 4096)) * 4096, SEEK_SET);
        if (read(fd, buf, 4096) != 4096)
            return -4;
    }
    close(fd);

    fd = open(path_big2, O_RDONLY);
    if (fd < 0)
        return -5;
    begin(4);
    for (uint32_t i = 0; i < 512; i++) {
        lseek(fd, (long)((i * 7919u) % (FILE_SZ / 4096)) * 4096, SEEK_SET);
        if (read(fd, buf, 4096) != 4096)
            return -6;
    }
    close(fd);

    fd = open3(path_w, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    begin(5);
    if (fd >= 0) {
        memset(buf, 'w', CHUNK);
        for (int i = 0; i < 64; i++)
            if (write(fd, buf, CHUNK) != CHUNK)
                return -7;
        syscall3(SYS_fsync, fd, 0, 0);
        close(fd);
    }
    begin(0xFF);
    if (fd >= 0)
        syscall3(SYS_unlink, (long)path_w, 0, 0);
    return fd >= 0 ? 0 : 1;
}

/* --- Linux as /init ------------------------------------------------------ */

static const char *linux_setup(void)
{
    syscall5(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
    int con = open("/dev/ttyS0", O_RDWR);
    dup2(con, 1);
    dup2(con, 2);
    static char list[4096];
    int lf = open("/modules.list", O_RDONLY);
    long n = lf >= 0 ? read(lf, list, sizeof list - 1) : 0;
    list[n > 0 ? n : 0] = 0;
    for (char *p = list; *p; ) {
        char *e = p;
        while (*e && *e != '\n')
            e++;
        char c = *e;
        *e = 0;
        if (*p) {
            int ko = open(p, O_RDONLY);
            if (ko < 0 || syscall3(SYS_finit_module, ko, (long)"", 0) < 0) {
                put("diskbench: could not load ");
                put(p);
                put("\n");
            }
            close(ko);
        }
        p = c ? e + 1 : e;
    }
    static const char *devs[] = { "/dev/sda", "/dev/vda", "/dev/nvme0n1" };
    for (int tries = 0; tries < 100; tries++) {
        for (int i = 0; i < 3; i++)
            if (syscall5(SYS_mount, (long)devs[i], (long)"/mnt", (long)"ext2", 0, 0) == 0) {
                put("diskbench: mounted ");
                put(devs[i]);
                put("\n");
                return "/mnt";
            }
        msleep(100);                /* the disk may still be probing */
    }
    put("diskbench: no disk to mount\n");
    return 0;
}

int main(int argc, char **argv)
{
    int is_init = getpid() == 1;
    const char *dir = argc > 1 ? argv[1] : "/data";
    if (is_init)
        dir = linux_setup();
    if (!dir)
        goto out;
    join(path_big, dir, "big");
    join(path_big2, dir, "big2");
    join(path_w, dir, "w");

    int cmd[2], res[2];
    if (pipe(cmd) < 0 || pipe(res) < 0) {
        put("diskbench: no pipe\n");
        goto out;
    }
    fcntl(cmd[0], F_SETFL, O_NONBLOCK);
    int pid = fork();
    if (pid == 0)
        spinner(cmd[0], res[1]);
    cmdw = cmd[1];
    int rc = work();
    if (rc < 0) {
        put("diskbench: failed at step ");
        put_u64((uint64_t)-rc);
        put("\n");
        uint8_t stop = 0xFF;
        write(cmdw, &stop, 1);
    }
    struct result r;
    long got = 0;
    while (got < (long)sizeof r) {
        long k = read(res[0], (uint8_t *)&r + got, sizeof r - got);
        if (k <= 0)
            break;
        got += k;
    }
    wait4(pid, 0, 0, 0);
    if (got != (long)sizeof r || rc < 0)
        goto out;
    /* Timer ticks and such, per cycle of elapsed time. */
    uint64_t base_num = r.gaps[0], base_den = r.elapsed[0] ? r.elapsed[0] : 1;
    for (int p = 1; p < NPHASE; p++) {
        if (p == 5 && rc == 1)
            continue;
        uint64_t idle = base_num * r.elapsed[p] / base_den;
        uint64_t work_c = r.gaps[p] > idle ? r.gaps[p] - idle : 0;
        put_line(phase_name[p], work_c / phase_ops[p], " cycles/op");
    }
    put_line("timer ticks etc. (per million)", base_num * 1000000 / base_den, "");
out:
    put("diskbench: done\n");
    if (is_init)
        syscall3(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc);
    return 0;
}
