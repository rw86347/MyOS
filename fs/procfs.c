/*
 * procfs.c — /proc, the kernel's own information shown as files
 *
 * On Linux, /proc is how programs ask the kernel about processes: `ps`
 * reads /proc/<pid>/stat, `free` reads /proc/meminfo. Nothing is stored;
 * each file is made up when it's read.
 *
 * The first user is musl's ttyname(): "which terminal is fd 0?" It reads
 * the link /proc/self/fd/0 (say "/dev/tty1"), then checks that stat() of
 * that name is the same file as fstat(0). `login` refuses to run without
 * a terminal name. So for now /proc holds just that:
 *
 *   /proc/self/fd/0 ... 63    one link per fd slot (a listing shows only
 *                             the caller's open ones)
 *   /proc/self/exe            a link to the caller's program (Firefox,
 *                             and anything that asks "where am I?")
 *   /proc/self/task/          its link count is 2 + the caller's threads:
 *                             Chromium's zygote checks it is single-
 *                             threaded (3) before it forks. Empty for now.
 *   /proc/cmdline             the kernel's command line: for now the one
 *                             built in (make CMDLINE=...), e.g. "nodesktop",
 *                             which the login service reads
 *   /proc/cpuinfo, meminfo    made up when read, in Linux's format (free,
 *                             and Chromium's CPU-feature library, read them)
 *   /proc/sys/fs/inotify/     the inotify limits (fs/inotify.c's numbers)
 *
 * The tree is built once, at mount time, from ordinary RAM nodes. What's
 * special is what a link points to: "self" means whoever is looking, so
 * each fd link is marked I_PROC_FD and vfs.c asks the CALLER's fd table
 * when the link is used. /proc/self/exe is I_PROC_EXE and asks the
 * caller's process. Looking one up is O(1) — no search, nothing
 * allocated — and there is nothing to keep up to date as processes come
 * and go. (Linux makes self a link to /proc/<pid>; that comes with the
 * per-process directories, ROADMAP 8j.)
 */
#include <blockdev.h>
#include <cpu.h>
#include <cycles.h>
#include <errno.h>
#include <ipv4.h>
#include <interrupts.h>
#include <kheap.h>
#include <mm.h>
#include <netdevice.h>
#include <pcache.h>
#include <pmm.h>
#include <proc.h>
#include <random.h>
#include <string.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <vfs.h>

#ifndef MYOS_CMDLINE
#define MYOS_CMDLINE "BOOT_IMAGE=myos"    /* never empty, as on Linux */
#endif
static const char cmdline_text[] = MYOS_CMDLINE "\n";

/* The links' names, "0".."63": nodes point at their name, not a copy.
   Only the built-in 64 (PROC_FILES_BASE) are listed: a node each for all
   1024 would cost ~150 KiB of inodes (docs/gaps.md). */
static char fd_names[PROC_FILES_BASE][3];

/*
 * /proc/net/dns — the nameserver, in resolv.conf's own format, so bringing
 * the machine up is `cp /proc/net/dns /etc/resolv.conf` and no script has to
 * know how to format it.
 *
 * It exists because the address is no longer a constant. The kernel is the
 * DHCP client (net/dhcp.c), so it is the only thing that knows which
 * nameserver this network has, and musl reads /etc/resolv.conf as an ordinary
 * file. A hard-coded resolv.conf naming QEMU's 10.0.2.3 is right in QEMU and
 * wrong everywhere else, which is exactly the machine DHCP was added for.
 *
 * Made when it is read, from the kernel's nameserver of the moment: on a
 * laptop the lease comes from Wi-Fi seconds after init has started (and again
 * after every reconnect), so a copy made at mount time would say "0.0.0.0"
 * for good. /etc/resolv.conf is a link to this file (etc/runit/1), and musl
 * reads resolv.conf afresh for every lookup.
 */
static char dns_text[32];

/* "nameserver 1.2.3.4\n". Written by hand: the kernel has no snprintf, and
   this wants a length back rather than a console. */
static uint32_t fmt_dns(uint32_t ip)
{
    static const char label[] = "nameserver ";
    uint32_t n = sizeof label - 1;
    memcpy(dns_text, label, n);
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint32_t b = (ip >> shift) & 0xFF;
        if (b >= 100)
            dns_text[n++] = '0' + b / 100;
        if (b >= 10)
            dns_text[n++] = '0' + (b / 10) % 10;
        dns_text[n++] = '0' + b % 10;
        if (shift)
            dns_text[n++] = '.';
    }
    dns_text[n++] = '\n';
    return n;                           /* at most 11 + 15 + 1 = 27 */
}

/* --- files made up when they are read ------------------------------------------ */

/* What a generated file is: its kind, and for /proc/<pid>/ files the pid
   (fs_ino = pid * PID_STEP + kind; pid 0 is whoever reads: /proc/self). */
enum {
    GEN_CPUINFO = 1, GEN_MEMINFO, GEN_DNS, GEN_ROUTE, GEN_UPTIME, GEN_LOADAVG, GEN_STAT,
    GEN_PARTITIONS, GEN_HOSTNAME, GEN_UUID, GEN_BOOTID, GEN_NETDEV,
    P_STAT = 64, P_STATUS, P_STATM, P_CMDLINE, P_COMM, P_ENVIRON, P_MAPS, P_LIMITS,
    P_MOUNTS, P_MOUNTINFO, P_CGROUP, P_OOM, P_IO, P_WCHAN,
};
#define PID_STEP 128
#define GEN_MAX  4096
#define GEN_BIG  65536                  /* maps, mountinfo: a browser has thousands of areas */

static struct inode *proc_root;          /* the last /proc mounted: pids come and go here */

/* A little text builder (the kernel has no snprintf). */
struct gen { char *p; uint32_t n, cap; };

static void put(struct gen *g, const char *s)
{
    while (*s && g->n < g->cap)
        g->p[g->n++] = *s++;
}

static void put_u(struct gen *g, uint32_t v)
{
    char b[11];
    int i = 10;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10);
    while (v /= 10);
    put(g, b + i);
}

static void put_ul(struct gen *g, uint64_t v)
{
    char b[21];
    int i = 20;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10);
    while (v /= 10);
    put(g, b + i);
}

static void put_hexw(struct gen *g, uint64_t v, int width)
{
    char b[17];
    int i = 16;
    b[i] = 0;
    do {
        b[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while ((v || 16 - i < width) && i > 0);
    put(g, b + i);
}

static void put_n(struct gen *g, const char *s, uint32_t n)
{
    for (uint32_t k = 0; k < n && g->n < g->cap; k++)
        g->p[g->n++] = s[k];
}

/* "Label:   1234 kB": the number right-aligned at column 24, as Linux does. */
static void put_kb(struct gen *g, const char *label, uint32_t kb)
{
    char num[11];
    struct gen n = { num, 0, sizeof num - 1 };
    put_u(&n, kb);
    num[n.n] = 0;
    put(g, label);
    put(g, ":");
    for (uint32_t w = (uint32_t)strlen(label) + 1 + n.n; w < 24; w++)
        put(g, " ");
    put(g, num);
    put(g, " kB\n");
}

static void meminfo(struct gen *g)
{
    uint32_t total = pmm_usable_count() * 4, free = pmm_free_count() * 4;
    uint32_t cached = pcache_pages() * 4;
    put_kb(g, "MemTotal", total);
    put_kb(g, "MemFree", free);
    put_kb(g, "MemAvailable", free + cached);   /* clean cache pages come back on demand */
    put_kb(g, "Buffers", 0);
    put_kb(g, "Cached", cached);
    put_kb(g, "SwapCached", 0);
    put_kb(g, "SwapTotal", 0);                  /* no swap, by design */
    put_kb(g, "SwapFree", 0);
    put_kb(g, "Shmem", 0);
}

/* The flags line: CPUID bits under Linux's names (arch/x86/include/asm/cpufeatures.h). */
struct flag { uint8_t reg, bit; const char *name; };
enum { F1_EDX, F1_ECX, F7_EBX, F7_ECX, X1_EDX, X1_ECX };
static const struct flag flags[] = {
    { F1_EDX, 0, "fpu" }, { F1_EDX, 1, "vme" }, { F1_EDX, 2, "de" }, { F1_EDX, 3, "pse" },
    { F1_EDX, 4, "tsc" }, { F1_EDX, 5, "msr" }, { F1_EDX, 6, "pae" }, { F1_EDX, 7, "mce" },
    { F1_EDX, 8, "cx8" }, { F1_EDX, 9, "apic" }, { F1_EDX, 11, "sep" }, { F1_EDX, 12, "mtrr" },
    { F1_EDX, 13, "pge" }, { F1_EDX, 14, "mca" }, { F1_EDX, 15, "cmov" }, { F1_EDX, 16, "pat" },
    { F1_EDX, 17, "pse36" }, { F1_EDX, 19, "clflush" }, { F1_EDX, 23, "mmx" },
    { F1_EDX, 24, "fxsr" }, { F1_EDX, 25, "sse" }, { F1_EDX, 26, "sse2" }, { F1_EDX, 28, "ht" },
    { X1_EDX, 11, "syscall" }, { X1_EDX, 20, "nx" }, { X1_EDX, 26, "pdpe1gb" },
    { X1_EDX, 27, "rdtscp" }, { X1_EDX, 29, "lm" },
    { F1_ECX, 0, "pni" }, { F1_ECX, 1, "pclmulqdq" }, { F1_ECX, 3, "monitor" },
    { F1_ECX, 9, "ssse3" }, { F1_ECX, 12, "fma" }, { F1_ECX, 13, "cx16" }, { F1_ECX, 17, "pcid" },
    { F1_ECX, 19, "sse4_1" }, { F1_ECX, 20, "sse4_2" }, { F1_ECX, 22, "movbe" },
    { F1_ECX, 23, "popcnt" }, { F1_ECX, 25, "aes" }, { F1_ECX, 26, "xsave" },
    { F1_ECX, 28, "avx" }, { F1_ECX, 29, "f16c" }, { F1_ECX, 30, "rdrand" },
    { F1_ECX, 31, "hypervisor" },
    { X1_ECX, 0, "lahf_lm" }, { X1_ECX, 5, "abm" }, { X1_ECX, 8, "3dnowprefetch" },
    { F7_EBX, 0, "fsgsbase" }, { F7_EBX, 3, "bmi1" }, { F7_EBX, 5, "avx2" },
    { F7_EBX, 7, "smep" }, { F7_EBX, 8, "bmi2" }, { F7_EBX, 9, "erms" },
    { F7_EBX, 10, "invpcid" }, { F7_EBX, 16, "avx512f" }, { F7_EBX, 18, "rdseed" },
    { F7_EBX, 19, "adx" }, { F7_EBX, 20, "smap" }, { F7_EBX, 23, "clflushopt" },
    { F7_EBX, 29, "sha_ni" },
    { F7_ECX, 1, "avx512vbmi" }, { F7_ECX, 2, "umip" }, { F7_ECX, 9, "vaes" },
    { F7_ECX, 10, "vpclmulqdq" }, { F7_ECX, 22, "rdpid" },
};

/* Features whose registers the kernel must save for programs (XSAVE state):
   listed only if the kernel switched XSAVE on (CPUID.1:ECX.OSXSAVE), as on
   Linux; otherwise a program trusting the list would lose its AVX registers
   at the next task switch. */
static int needs_xsave(const struct flag *f)
{
    if (f->reg == F1_ECX)
        return f->bit == 12 || f->bit == 26 || f->bit == 28 || f->bit == 29;
    if (f->reg == F7_EBX)
        return f->bit == 5 || f->bit == 16;
    return f->reg == F7_ECX && (f->bit == 1 || f->bit == 9 || f->bit == 10);
}

static void cpuinfo(struct gen *g)
{
    uint32_t r[4], max, xmax, regs[6] = { 0 };
    char vendor[13];
    cpu_cpuid(0, 0, r);
    max = r[0];
    memcpy(vendor, &r[1], 4), memcpy(vendor + 4, &r[3], 4), memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    cpu_cpuid(1, 0, r);
    uint32_t sig = r[0];
    regs[F1_EDX] = r[3], regs[F1_ECX] = r[2];
    if (max >= 7) {
        cpu_cpuid(7, 0, r);
        regs[F7_EBX] = r[1], regs[F7_ECX] = r[2];
    }
    cpu_cpuid(0x80000000, 0, r);
    xmax = r[0];
    if (xmax >= 0x80000001) {
        cpu_cpuid(0x80000001, 0, r);
        regs[X1_EDX] = r[3], regs[X1_ECX] = r[2];
    }
    char brand[49] = "unknown";
    if (xmax >= 0x80000004) {
        for (uint32_t k = 0; k < 3; k++) {
            cpu_cpuid(0x80000002 + k, 0, r);
            memcpy(brand + 16 * k, r, 16);
        }
        brand[48] = 0;
    }
    const char *name = brand;
    while (*name == ' ')
        name++;
    uint32_t family = (sig >> 8) & 0xF, model = (sig >> 4) & 0xF;
    if (family == 0xF)
        family += (sig >> 20) & 0xFF;
    if (family == 6 || family >= 0xF)
        model |= ((sig >> 16) & 0xF) << 4;
    uint32_t mhz = cycles_per_us();
    int osxsave = (regs[F1_ECX] >> 27) & 1;

    put(g, "processor\t: 0\nvendor_id\t: "), put(g, vendor);
    put(g, "\ncpu family\t: "), put_u(g, family);
    put(g, "\nmodel\t\t: "), put_u(g, model);
    put(g, "\nmodel name\t: "), put(g, name);
    put(g, "\nstepping\t: "), put_u(g, sig & 0xF);
    put(g, "\ncpu MHz\t\t: "), put_u(g, mhz), put(g, ".000");
    put(g, "\nphysical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1"
           "\napicid\t\t: 0\ninitial apicid\t: 0\nfpu\t\t: yes\nfpu_exception\t: yes"
           "\ncpuid level\t: "), put_u(g, max);
    put(g, "\nwp\t\t: yes\nflags\t\t:");
    for (uint32_t k = 0; k < sizeof flags / sizeof flags[0]; k++)
        if (((regs[flags[k].reg] >> flags[k].bit) & 1) && (osxsave || !needs_xsave(&flags[k])))
            put(g, " "), put(g, flags[k].name);
    put(g, "\nbogomips\t: "), put_u(g, 2 * mhz), put(g, ".00");
    put(g, "\nclflush size\t: 64\ncache_alignment\t: 64");
    if (xmax >= 0x80000008) {
        cpu_cpuid(0x80000008, 0, r);
        put(g, "\naddress sizes\t: "), put_u(g, r[0] & 0xFF);
        put(g, " bits physical, "), put_u(g, (r[0] >> 8) & 0xFF), put(g, " bits virtual");
    }
    put(g, "\npower management:\n\n");
}

static void dnsinfo(struct gen *g)
{
    if (!ipv4_dns) {
        put(g, "# no nameserver yet: DHCP has not answered on this network\n");
        return;
    }
    uint32_t n = fmt_dns(ipv4_dns);
    for (uint32_t k = 0; k < n && g->n < g->cap; k++)
        g->p[g->n++] = dns_text[k];
}

/* A 32-bit value as Linux's /proc/net/route shows an address: the bytes as
   they travel (network order), printed as a little-endian word. */
static void put_hex8(struct gen *g, uint32_t ip)
{
    uint32_t v = (ip >> 24) | ((ip >> 8) & 0xFF00) | ((ip << 8) & 0xFF0000) | (ip << 24);
    for (int shift = 28; shift >= 0; shift -= 4)
        if (g->n < g->cap)
            g->p[g->n++] = "0123456789ABCDEF"[(v >> shift) & 0xF];
}

/* /proc/net/route in Linux's format (busybox `route -n` and `netstat -r` read
   it): the default route through the gateway, and the local subnet. */
static void routeinfo(struct gen *g)
{
    put(g, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    struct net_device *dev = ipv4_dev();
    if (!dev || !ipv4_addr)
        return;
    for (int def = 1; def >= 0; def--) {
        if (def && !ipv4_gw)
            continue;
        put(g, dev->name), put(g, "\t");
        put_hex8(g, def ? 0 : ipv4_addr & ipv4_mask), put(g, "\t");
        put_hex8(g, def ? ipv4_gw : 0), put(g, def ? "\t0003" : "\t0001");
        put(g, "\t0\t0\t0\t");
        put_hex8(g, def ? 0 : ipv4_mask), put(g, "\t0\t0\t0\n");
    }
}

/* --- the system as a whole ------------------------------------------------------ */

static uint32_t idle_ms(void)
{
    uint32_t mhz = cycles_per_us();
    return mhz ? (uint32_t)(cpu_idle_cycles() / mhz / 1000) : 0;
}

static void two_dec(struct gen *g, uint32_t ms)    /* "12.34" seconds */
{
    put_u(g, ms / 1000);
    put(g, ".");
    uint32_t cs = ms % 1000 / 10;
    if (cs < 10)
        put(g, "0");
    put_u(g, cs);
}

static void uptime(struct gen *g)
{
    two_dec(g, timer_ms());
    put(g, " ");
    two_dec(g, idle_ms());
    put(g, "\n");
}

/* Program threads (not the kernel's own): all of them, or those ready to run. */
static uint32_t user_threads(int ready_only)
{
    uint32_t n = 0;
    for (int i = 0; i < THREAD_MAX; i++) {
        struct thread *t = thread_at(i);
        if (t->proc && (t->state == THREAD_READY || (!ready_only && t->state == THREAD_BLOCKED)))
            n++;
    }
    return n;
}

/* One CPU, and no history kept: the load is the program threads ready to
   run right now (the reader among them), the same in all three columns. */
static void loadavg(struct gen *g)
{
    uint32_t r = user_threads(1);
    r -= r > 0;                         /* not counting whoever is reading */
    for (int k = 0; k < 3; k++) {
        put_u(g, r);
        put(g, ".00 ");
    }
    put_u(g, r);
    put(g, "/");
    put_u(g, user_threads(0));
    put(g, " ");
    put_u(g, (uint32_t)(proc_next_pid() - 1));
    put(g, "\n");
}

/* CPU time in ticks of 1/100 s (USER_HZ) since boot: top's %CPU. */
static void stat_all(struct gen *g)
{
    uint64_t user = 0, sys = 0;
    int nproc = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state == PROC_FREE)
            continue;
        nproc++;
        user += p->utime + p->cutime;
        sys += p->stime + p->cstime;
        for (struct thread *t = p->threads; t; t = t->proc_next) {
            user += t->utime;
            sys += t->stime;
        }
    }
    uint64_t idle = idle_ms();
    for (int cpu = -1; cpu < 1; cpu++) {
        put(g, cpu < 0 ? "cpu  " : "cpu0 ");
        put_ul(g, user / 10), put(g, " 0 ");
        put_ul(g, sys / 10), put(g, " ");
        put_ul(g, idle / 10), put(g, " 0 0 0 0 0 0\n");
    }
    put(g, "intr 0\nctxt 0\nbtime ");
    put_u(g, time_now() - timer_ms() / 1000);
    put(g, "\nprocesses ");
    put_u(g, (uint32_t)(proc_next_pid() - 1));
    put(g, "\nprocs_running ");
    put_u(g, user_threads(1));
    put(g, "\nprocs_blocked 0\nsoftirq 0 0 0 0 0 0 0 0 0 0 0\n");
    (void)nproc;
}

static void partitions(struct gen *g)
{
    put(g, "major minor  #blocks  name\n\n");
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_get(i);
        if (!d || !d->name[0] || !d->sectors)
            continue;
        unsigned ma, mi;
        blockdev_devnum(d, &ma, &mi);
        put(g, ma < 10 ? "   " : ma < 100 ? "  " : " "), put_u(g, ma);
        put(g, "  "), put_u(g, mi);
        put(g, " "), put_u(g, d->sectors / 2);
        put(g, " "), put(g, d->name), put(g, "\n");
    }
}

static void uuid(struct gen *g, int boot)
{
    static uint8_t boot_id[16];
    static int have;
    uint8_t u[16];
    if (boot) {
        if (!have) {
            random_bytes(boot_id, sizeof boot_id);
            have = 1;
        }
        memcpy(u, boot_id, sizeof u);
    } else
        random_bytes(u, sizeof u);
    u[6] = (uint8_t)((u[6] & 0x0F) | 0x40);         /* version 4: random */
    u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);
    for (int k = 0; k < 16; k++) {
        if (k == 4 || k == 6 || k == 8 || k == 10)
            put(g, "-");
        put_hexw(g, u[k], 2);
    }
    put(g, "\n");
}

static void netdev(struct gen *g)
{
    put(g, "Inter-|   Receive                                                |  Transmit\n"
           " face |bytes    packets errs drop fifo frame compressed multicast|"
           "bytes    packets errs drop fifo colls carrier compressed\n");
    for (int k = 1; k < 32; k++) {
        struct net_device *d = dev_get_by_index(k);
        if (!d)
            continue;
        put(g, "  "), put(g, d->name), put(g, ": ");
        put_ul(g, d->stats.rx_bytes), put(g, " "), put_ul(g, d->stats.rx_packets);
        put(g, " 0 0 0 0 0 0 ");
        put_ul(g, d->stats.tx_bytes), put(g, " "), put_ul(g, d->stats.tx_packets);
        put(g, " 0 0 0 0 0 0\n");
    }
}

/* --- one process: /proc/<pid>/ and /proc/self/ ----------------------------------- */

static char state_of(struct process *p)
{
    if (p->state == PROC_ZOMBIE)
        return 'Z';
    for (struct thread *t = p->threads; t; t = t->proc_next)
        if (t->state == THREAD_READY)
            return 'R';
    return 'S';
}

/* The main thread's name (tid == pid); Xorg's newest thread is "InputThread". */
static const char *comm_of(struct process *p)
{
    for (struct thread *t = p->threads; t; t = t->proc_next)
        if (t->tid == p->pid)
            return t->name;
    return p->threads ? p->threads->name : "zombie";
}

static uint64_t vsize_of(struct process *p)
{
    uint64_t v = 0;
    if (p->mm && p->state != PROC_ZOMBIE)
        for (int k = 0; k < p->mm->nareas; k++)
            v += p->mm->area[k].end - p->mm->area[k].start;
    return v;
}

static void cpu_of(struct process *p, uint32_t *user, uint32_t *sys)
{
    *user = p->utime;
    *sys = p->stime;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        *user += t->utime;
        *sys += t->stime;
    }
}

/* Linux's 52 fields; what MyOS doesn't count reads 0. */
static void pid_stat(struct gen *g, struct process *p)
{
    uint32_t user, sys;
    cpu_of(p, &user, &sys);
    put_u(g, (uint32_t)p->pid), put(g, " ("), put(g, comm_of(p)), put(g, ") ");
    char st[2] = { state_of(p), 0 };
    put(g, st), put(g, " ");
    put_u(g, (uint32_t)(p->parent ? p->parent->pid : 0)), put(g, " ");
    put_u(g, (uint32_t)p->pgid), put(g, " "), put_u(g, (uint32_t)p->sid);
    put(g, " 0 -1 4194304 0 0 0 0 ");                   /* tty, tpgid, flags, faults */
    put_u(g, user / 10), put(g, " "), put_u(g, sys / 10), put(g, " ");
    put_u(g, p->cutime / 10), put(g, " "), put_u(g, p->cstime / 10);
    put(g, " 20 0 "), put_u(g, (uint32_t)(p->nthreads ? p->nthreads : 1));
    put(g, " 0 "), put_u(g, p->start_ms / 10), put(g, " ");
    put_ul(g, vsize_of(p));
    put(g, " 0 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
}

static void pid_status(struct gen *g, struct process *p)
{
    put(g, "Name:\t"), put(g, comm_of(p));
    put(g, "\nUmask:\t0"), put_u(g, p->umask >> 6 & 7), put_u(g, p->umask >> 3 & 7), put_u(g, p->umask & 7);
    char st[2] = { state_of(p), 0 };
    put(g, "\nState:\t"), put(g, st);
    put(g, st[0] == 'R' ? " (running)" : st[0] == 'Z' ? " (zombie)" : " (sleeping)");
    put(g, "\nTgid:\t"), put_u(g, (uint32_t)p->pid);
    put(g, "\nNgid:\t0\nPid:\t"), put_u(g, (uint32_t)p->pid);
    put(g, "\nPPid:\t"), put_u(g, (uint32_t)(p->parent ? p->parent->pid : 0));
    put(g, "\nTracerPid:\t0\nUid:\t");
    put_u(g, p->cred.uid), put(g, "\t"), put_u(g, p->cred.euid), put(g, "\t");
    put_u(g, p->cred.suid), put(g, "\t"), put_u(g, p->cred.fsuid);
    put(g, "\nGid:\t");
    put_u(g, p->cred.gid), put(g, "\t"), put_u(g, p->cred.egid), put(g, "\t");
    put_u(g, p->cred.sgid), put(g, "\t"), put_u(g, p->cred.fsgid);
    put(g, "\nFDSize:\t"), put_u(g, p->nfiles);
    put(g, "\nGroups:\t\n");
    put_kb(g, "VmPeak", (uint32_t)(vsize_of(p) / 1024));
    put_kb(g, "VmSize", (uint32_t)(vsize_of(p) / 1024));
    put_kb(g, "VmRSS", 0);
    put(g, "Threads:\t"), put_u(g, (uint32_t)(p->nthreads ? p->nthreads : 1));
    put(g, "\nSigQ:\t0/0\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n"
           "Cpus_allowed:\t1\nCpus_allowed_list:\t0\nvoluntary_ctxt_switches:\t0\n"
           "nonvoluntary_ctxt_switches:\t0\n");
}

static void pid_maps(struct gen *g, struct process *p)
{
    if (!p->mm || p->state == PROC_ZOMBIE)
        return;
    char path[256];
    for (int k = 0; k < p->mm->nareas; k++) {
        struct vma *v = &p->mm->area[k];
        put_hexw(g, v->start, 8), put(g, "-"), put_hexw(g, v->end, 8), put(g, " ");
        char perm[5] = { v->flags & VM_READ ? 'r' : '-', v->flags & VM_WRITE ? 'w' : '-',
                         v->flags & VM_EXEC ? 'x' : '-', v->flags & VM_SHARED ? 's' : 'p', 0 };
        put(g, perm), put(g, " ");
        put_hexw(g, v->file ? v->file_off : 0, 8);
        put(g, " 00:00 "), put_u(g, v->file ? v->file->ino : 0);
        if (v->file && vfs_path(v->file, path, sizeof path) > 0)
            put(g, "                          "), put(g, path);
        else if (p->mm->brk_start && v->start == (p->mm->brk_start & ~(user_addr_t)(PAGE_SIZE - 1)))
            put(g, "                          [heap]");
        put(g, "\n");
    }
}

static void pid_limits(struct gen *g)
{
    put(g, "Limit                     Soft Limit           Hard Limit           Units     \n"
           "Max cpu time              unlimited            unlimited            seconds   \n"
           "Max file size             unlimited            unlimited            bytes     \n"
           "Max data size             unlimited            unlimited            bytes     \n"
           "Max stack size            8388608              unlimited            bytes     \n"
           "Max core file size        0                    unlimited            bytes     \n"
           "Max resident set          unlimited            unlimited            bytes     \n"
           "Max processes             unlimited            unlimited            processes \n"
           "Max open files            1024                 1024                 files     \n"
           "Max locked memory         unlimited            unlimited            bytes     \n"
           "Max address space         unlimited            unlimited            bytes     \n");
}

/* The mount table: the disks (ext2, FAT) and the RAM ones mount(2) made. */
static void mounts(struct gen *g, int info)
{
    char path[256];
    int id = 20;
    for (int k = 0; k < vfs_mount_count(); k++) {
        struct mount *m = vfs_mount_at(k);
        if (!m || !m->root || vfs_path(m->root, path, sizeof path) <= 0)
            continue;
        const char *src = m->source[0] ? m->source : m->type;
        if (info) {
            put_u(g, (uint32_t)id++), put(g, " 1 0:"), put_u(g, m->dev_minor);
            put(g, " / "), put(g, path), put(g, " rw,relatime - "), put(g, m->type);
            put(g, " "), put(g, src), put(g, " rw\n");
        } else {
            put(g, src), put(g, " "), put(g, path), put(g, " "), put(g, m->type);
            put(g, " rw,relatime 0 0\n");
        }
    }
    for (int k = 0; k < vfs_noted_count(); k++) {
        const char *where, *type;
        vfs_noted_at(k, &where, &type);
        if (info) {
            put_u(g, (uint32_t)id++), put(g, " 1 0:1 / "), put(g, where);
            put(g, " rw,nosuid,nodev - "), put(g, type), put(g, " "), put(g, type), put(g, " rw\n");
        } else {
            put(g, type), put(g, " "), put(g, where), put(g, " "), put(g, type);
            put(g, " rw,nosuid,nodev 0 0\n");
        }
    }
}

static int32_t gen_read(struct file *f, void *ubuf, uint32_t n)
{
    uint32_t kind = f->inode->fs_ino % PID_STEP, pid = f->inode->fs_ino / PID_STEP;
    struct process *p = NULL;
    if (kind >= P_STAT) {
        p = pid ? proc_find((int)pid) : proc_current();
        if (!p)
            return -ESRCH;              /* it ended while the file was open */
        /* Where a program's memory is (maps) undoes its address
           randomization: only its own user and root may look, as on Linux. */
        struct process *me = proc_current();
        if ((kind == P_MAPS || kind == P_ENVIRON || kind == P_IO) && me && p != me &&
            me->cred.euid != 0 && me->cred.euid != p->cred.uid)
            return -EACCES;
    }
    uint32_t cap = kind == P_MAPS || kind == P_MOUNTINFO ? GEN_BIG : GEN_MAX;
    char *buf = kmalloc(cap);
    if (!buf)
        return -ENOMEM;
    struct gen g = { buf, 0, cap };
    switch (kind) {
    case GEN_CPUINFO:    cpuinfo(&g); break;
    case GEN_MEMINFO:    meminfo(&g); break;
    case GEN_DNS:        dnsinfo(&g); break;
    case GEN_ROUTE:      routeinfo(&g); break;
    case GEN_UPTIME:     uptime(&g); break;
    case GEN_LOADAVG:    loadavg(&g); break;
    case GEN_STAT:       stat_all(&g); break;
    case GEN_PARTITIONS: partitions(&g); break;
    case GEN_HOSTNAME:   put(&g, uts_nodename()), put(&g, "\n"); break;
    case GEN_UUID:       uuid(&g, 0); break;
    case GEN_BOOTID:     uuid(&g, 1); break;
    case GEN_NETDEV:     netdev(&g); break;
    case P_STAT:         pid_stat(&g, p); break;
    case P_STATUS:       pid_status(&g, p); break;
    case P_STATM:        put_ul(&g, vsize_of(p) / PAGE_SIZE), put(&g, " 0 0 0 0 0 0\n"); break;
    case P_CMDLINE:      put_n(&g, p->cmdline, p->cmdline_len); break;
    case P_COMM:         put(&g, comm_of(p)), put(&g, "\n"); break;
    case P_ENVIRON:      break;      /* not kept: a program's own copy is in its memory */
    case P_MAPS:         pid_maps(&g, p); break;
    case P_LIMITS:       pid_limits(&g); break;
    case P_MOUNTS:       mounts(&g, 0); break;
    case P_MOUNTINFO:    mounts(&g, 1); break;
    case P_CGROUP:       put(&g, "0::/\n"); break;
    case P_OOM:          put(&g, "0\n"); break;
    case P_IO:           put(&g, "rchar: 0\nwchar: 0\nsyscr: 0\nsyscw: 0\nread_bytes: 0\n"
                                 "write_bytes: 0\ncancelled_write_bytes: 0\n"); break;
    case P_WCHAN:        put(&g, "0"); break;
    }
    int32_t r = 0;
    if (f->pos < g.n) {
        r = (int32_t)(n < g.n - f->pos ? n : g.n - f->pos);
        if (copy_to_user(ubuf, buf + f->pos, (uint32_t)r) < 0)
            r = -EFAULT;
        else
            f->pos += (uint32_t)r;
    }
    kfree(buf);
    return r;
}

/* The mount tables are pollable as on Linux: always readable, and
   POLLERR | POLLPRI once after the table changed (a USB stick mounted).
   GLib's mount watcher waits on exactly that. f->priv holds the change
   count this open has seen (+1, so 0 never matches). */
#define POLL_IN_PRI_ERR (0x001 | 0x040 | 0x002 | 0x008)  /* IN | RDNORM | PRI | ERR */
static int is_mount_table(struct file *f)
{
    uint32_t kind = f->inode->fs_ino % PID_STEP;
    return kind == P_MOUNTS || kind == P_MOUNTINFO;
}

static int gen_open(struct file *f)
{
    if (is_mount_table(f))
        f->priv = (void *)(uintptr_t)(vfs_mount_gen() + 1);
    return 0;
}

static uint32_t gen_poll(struct file *f)
{
    if (!is_mount_table(f))
        return 0x001 | 0x004 | 0x040 | 0x100;       /* IN | OUT | RDNORM | WRNORM: Linux's default */
    uintptr_t now = (uintptr_t)vfs_mount_gen() + 1;
    if ((uintptr_t)f->priv == now)
        return 0x001 | 0x040;
    f->priv = (void *)now;              /* reported once per change */
    return POLL_IN_PRI_ERR;
}

static uint32_t gen_change_count(struct file *f)
{
    return is_mount_table(f) ? vfs_mount_gen() : 0;
}

/* /proc/<pid>/oom_score_adj takes a number, as on Linux (Chromium sets it
   for every renderer, so they die before the browser does when memory runs
   out). MyOS has no out-of-memory killer to use it yet: accepted, not kept.
   The other generated files can't be written. */
static int32_t gen_write(struct file *f, const void *ubuf, uint32_t n)
{
    (void)ubuf;
    return f->inode->fs_ino % PID_STEP == P_OOM ? (int32_t)n : -EINVAL;
}

static const struct file_ops gen_ops = {
    .read = gen_read, .write = gen_write, .open = gen_open, .poll = gen_poll,
    .change_count = gen_change_count,
};

/* /proc/sysrq-trigger, as on Linux: root writes a letter, the kernel acts.
   Only "t" so far: every thread and what it waits for (thread_dump, also
   Alt+SysRq+T on the keyboard). Mode 0200: only root may write it. */
static int32_t sysrq_write(struct file *f, const void *ubuf, uint32_t n)
{
    (void)f;
    char c = 0;
    if (n && copy_from_user(&c, ubuf, 1) < 0)
        return -EFAULT;
    if (c == 't')
        thread_dump();
    return (int32_t)n;
}

static const struct file_ops sysrq_ops = { .write = sysrq_write };

static const char watches_text[] = "4096\n", instances_text[] = "128\n", queued_text[] = "1024\n";

/* /proc/version, in Linux's words and with uname's release (sys_info.c).
   LibreOffice's launcher (oosplash) reads it to see that /proc is there,
   and stops with "/proc not mounted" if it isn't. */
static const char version_text[] = "Linux version 6.1.0-MyOS (myos@myos) (gcc) #1 MyOS\n";

/* A read-only file whose bytes are `text` (no copy). */
static struct inode *fixed(struct inode *dir, const char *name, const char *text, uint32_t len)
{
    struct inode *i = dir ? vfs_add(dir, name, (uint32_t)strlen(name), S_IFREG | 0444) : NULL;
    if (i) {
        i->size = len;
        i->data = (const uint8_t *)text;
    }
    return i;
}

static struct inode *gen_file(struct inode *dir, const char *name, uint32_t ino)
{
    struct inode *i = dir ? vfs_add(dir, name, (uint32_t)strlen(name), S_IFREG | 0444) : NULL;
    if (i) {
        i->ops = &gen_ops;              /* size 0, as on Linux: read to the end */
        i->fs_ino = ino;
    }
    return i;
}

static const struct { const char *name; uint32_t kind; } pid_files[] = {
    { "stat", P_STAT }, { "status", P_STATUS }, { "statm", P_STATM }, { "cmdline", P_CMDLINE },
    { "comm", P_COMM }, { "environ", P_ENVIRON }, { "maps", P_MAPS }, { "limits", P_LIMITS },
    { "mounts", P_MOUNTS }, { "mountinfo", P_MOUNTINFO }, { "cgroup", P_CGROUP },
    { "oom_score_adj", P_OOM }, { "io", P_IO }, { "wchan", P_WCHAN },
};

static void pid_files_in(struct inode *dir, uint32_t pid)
{
    for (uint32_t k = 0; k < sizeof pid_files / sizeof pid_files[0]; k++) {
        struct inode *i = gen_file(dir, pid_files[k].name, pid * PID_STEP + pid_files[k].kind);
        if (i && pid_files[k].kind == P_OOM)
            i->mode = S_IFREG | 0644;   /* the one a program writes */
    }
}

/* /proc/<pid>: made with the process (proc_alloc), gone when it is reaped.
   Each file is made up when read, so nothing here goes stale. */
void procfs_pid_add(int pid)
{
    if (!proc_root || pid <= 0 || (uint32_t)pid > 0xFFFFFFFFu / PID_STEP)
        return;
    char num[12];
    struct gen g = { num, 0, sizeof num - 1 };
    put_u(&g, (uint32_t)pid);
    unsigned long flags = irq_save();
    char *name = kmalloc(g.n);
    struct inode *dir = name && !vfs_child(proc_root, num, g.n)
                        ? vfs_add(proc_root, (memcpy(name, num, g.n), name), g.n, S_IFDIR | 0555) : NULL;
    if (dir) {
        dir->flags |= I_NAME_OWNED;
        pid_files_in(dir, (uint32_t)pid);
    } else
        kfree(name);
    irq_restore(flags);
}

void procfs_pid_remove(int pid)
{
    if (!proc_root)
        return;
    char num[12];
    struct gen g = { num, 0, sizeof num - 1 };
    put_u(&g, (uint32_t)pid);
    unsigned long flags = irq_save();
    struct inode *dir = vfs_child(proc_root, num, g.n);
    if (dir)
        vfs_remove(dir);
    irq_restore(flags);
}

static const char filesystems_text[] = "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\n"
                                       "nodev\tdevtmpfs\nnodev\tdevpts\n\text2\n\tvfat\n";
static const char swaps_text[] = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n";
static const char devices_text[] =
    "Character devices:\n  1 mem\n  4 /dev/vc/0\n  4 tty\n  5 /dev/tty\n  5 /dev/console\n"
    "  5 /dev/ptmx\n 13 input\n 29 fb\n116 alsa\n136 pts\n\n"
    "Block devices:\n  3 ide0\n  8 sd\n 22 ide1\n252 virtblk\n259 blkext\n";
static const char interrupts_text[] = "           CPU0\n";
static const char empty_text[] = "";
static const char osrelease_text[] = "6.1.0-MyOS\n", ostype_text[] = "Linux\n";
static const char pidmax_text[] = "32768\n", entropy_text[] = "256\n", zero_text[] = "0\n";
static const char filemax_text[] = "65536\n", somaxconn_text[] = "4096\n";
static const char tcp_text[] = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
                               "retrnsmt   uid  timeout inode\n";
static const char unix_text[] = "Num       RefCount Protocol Flags    Type St Inode Path\n";

int procfs_mount(const char *path)
{
    int err = vfs_mount_ram(path, 0555);
    if (err)
        return err;
    struct inode *proc, *self, *fd;
    err = vfs_lookup(path, &proc);      /* the new RAM directory on top */
    if (err)
        return err;
    self = vfs_add(proc, "self", 4, S_IFDIR | 0555);
    fd = self ? vfs_add(self, "fd", 2, S_IFDIR | 0555) : NULL;
    struct inode *exe = self ? vfs_add(self, "exe", 3, S_IFLNK | 0777) : NULL;
    struct inode *task = self ? vfs_add(self, "task", 4, S_IFDIR | 0555) : NULL;
    if (!fd || !exe || !task)
        return -ENOMEM;
    task->flags |= I_PROC_TASK;
    exe->flags |= I_PROC_EXE;
    exe->size = 64;                     /* Linux reports a path-sized link */
    for (int n = 0; n < PROC_FILES_BASE; n++) {
        char *s = fd_names[n];
        uint32_t len = n < 10 ? 1 : 2;
        s[0] = n < 10 ? '0' + n : '0' + n / 10;
        s[1] = '0' + n % 10;
        struct inode *l = vfs_add(fd, s, len, S_IFLNK | 0700);
        if (!l)
            return -ENOMEM;
        l->flags |= I_PROC_FD;
        l->fs_ino = n;
        l->size = 64;                   /* Linux shows that for these too */
    }
    struct inode *net = vfs_add(proc, "net", 3, S_IFDIR | 0555);
    struct inode *dns = net ? vfs_add(net, "dns", 3, S_IFREG | 0444) : NULL;
    struct inode *route = net ? vfs_add(net, "route", 5, S_IFREG | 0444) : NULL;
    if (!dns || !route)
        return -ENOMEM;
    dns->ops = route->ops = &gen_ops;   /* made when read: the lease may come later */
    dns->fs_ino = GEN_DNS;
    route->fs_ino = GEN_ROUTE;
    struct inode *cmdline = vfs_add(proc, "cmdline", 7, S_IFREG | 0444);
    if (!cmdline)
        return -ENOMEM;
    cmdline->size = sizeof cmdline_text - 1;
    cmdline->data = (const uint8_t *)cmdline_text;
    struct inode *cpu = vfs_add(proc, "cpuinfo", 7, S_IFREG | 0444);
    struct inode *mem = vfs_add(proc, "meminfo", 7, S_IFREG | 0444);
    if (!cpu || !mem)
        return -ENOMEM;
    cpu->ops = mem->ops = &gen_ops;     /* size 0, as on Linux: read to the end */
    cpu->fs_ino = GEN_CPUINFO;
    mem->fs_ino = GEN_MEMINFO;
    if (!fixed(proc, "version", version_text, sizeof version_text - 1))
        return -ENOMEM;
    struct inode *sq = vfs_add(proc, "sysrq-trigger", 13, S_IFREG | 0200);
    if (!sq)
        return -ENOMEM;
    sq->ops = &sysrq_ops;
    struct inode *sys = vfs_add(proc, "sys", 3, S_IFDIR | 0555);
    struct inode *kern = sys ? vfs_add(sys, "kernel", 6, S_IFDIR | 0555) : NULL;
    struct inode *rnd = kern ? vfs_add(kern, "random", 6, S_IFDIR | 0555) : NULL;
    struct inode *vm = sys ? vfs_add(sys, "vm", 2, S_IFDIR | 0555) : NULL;
    struct inode *snet = sys ? vfs_add(sys, "net", 3, S_IFDIR | 0555) : NULL;
    struct inode *core = snet ? vfs_add(snet, "core", 4, S_IFDIR | 0555) : NULL;
    if (!gen_file(proc, "uptime", GEN_UPTIME) || !gen_file(proc, "loadavg", GEN_LOADAVG) ||
        !gen_file(proc, "stat", GEN_STAT) || !gen_file(proc, "partitions", GEN_PARTITIONS) ||
        !gen_file(proc, "mounts", P_MOUNTS) ||
        !fixed(proc, "filesystems", filesystems_text, sizeof filesystems_text - 1) ||
        !fixed(proc, "swaps", swaps_text, sizeof swaps_text - 1) ||
        !fixed(proc, "devices", devices_text, sizeof devices_text - 1) ||
        !fixed(proc, "interrupts", interrupts_text, sizeof interrupts_text - 1) ||
        !fixed(proc, "modules", empty_text, 0) || !fixed(proc, "diskstats", empty_text, 0) ||
        !gen_file(kern, "hostname", GEN_HOSTNAME) ||
        !fixed(kern, "osrelease", osrelease_text, sizeof osrelease_text - 1) ||
        !fixed(kern, "ostype", ostype_text, sizeof ostype_text - 1) ||
        !fixed(kern, "pid_max", pidmax_text, sizeof pidmax_text - 1) ||
        !gen_file(rnd, "uuid", GEN_UUID) || !gen_file(rnd, "boot_id", GEN_BOOTID) ||
        !fixed(rnd, "entropy_avail", entropy_text, sizeof entropy_text - 1) ||
        !fixed(vm, "overcommit_memory", zero_text, sizeof zero_text - 1) ||
        !fixed(core, "somaxconn", somaxconn_text, sizeof somaxconn_text - 1) ||
        !gen_file(net, "dev", GEN_NETDEV) ||
        !fixed(net, "tcp", tcp_text, sizeof tcp_text - 1) ||
        !fixed(net, "udp", tcp_text, sizeof tcp_text - 1) ||
        !fixed(net, "unix", unix_text, sizeof unix_text - 1))
        return -ENOMEM;
    pid_files_in(self, 0);              /* /proc/self/stat ...: whoever reads */
    struct inode *fs = sys ? vfs_add(sys, "fs", 2, S_IFDIR | 0555) : NULL;
    if (fs && !fixed(fs, "file-max", filemax_text, sizeof filemax_text - 1))
        return -ENOMEM;
    proc_root = proc;
    for (int i = 0; i < PROC_MAX; i++)  /* the ones running already (init, ...) */
        if (proc_at(i)->state != PROC_FREE)
            procfs_pid_add(proc_at(i)->pid);
    struct inode *in = fs ? vfs_add(fs, "inotify", 7, S_IFDIR | 0555) : NULL;
    if (!fixed(in, "max_user_watches", watches_text, sizeof watches_text - 1) ||
        !fixed(in, "max_user_instances", instances_text, sizeof instances_text - 1) ||
        !fixed(in, "max_queued_events", queued_text, sizeof queued_text - 1))
        return -ENOMEM;
    return 0;
}

uint32_t procfs_nlink(const struct inode *i)
{
    if (i->flags & I_PROC_TASK) {
        struct process *p = proc_current();
        return 2 + (p ? (uint32_t)p->nthreads : 1);    /* ".", "..", a thread each */
    }
    return i->nlink;
}

/* The path of the file the caller's fd is open on, or of its program. */
int procfs_readlink(struct inode *link, char *buf, uint32_t size)
{
    if (link->flags & I_PROC_EXE) {
        struct process *p = proc_current();
        if (!p || !p->exe)
            return -ENOENT;
        return vfs_path(p->exe, buf, size);
    }
    struct file *f = fd_get(link->fs_ino);
    if (!f || !f->inode)
        return -ENOENT;
    return vfs_path(f->inode, buf, size);
}
