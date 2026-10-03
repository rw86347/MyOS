/*
 * bench.c — kernel benchmarks (only in `make bench` builds)
 *
 * Each benchmark repeats one operation many times and prints the average
 * cost in CPU cycles. Results go to docs/performance.md, so every speed
 * change has a before and after.
 *
 * In QEMU these numbers only compare versions of MyOS with each other: QEMU
 * emulates the CPU in software, so a "cycle" there isn't a real Pentium 4
 * cycle. Real numbers come from running `make bench` on the old hardware.
 */
#include <bcache.h>
#include <bench.h>
#include <blockdev.h>
#include <cycles.h>
#include <interrupts.h>
#include <io.h>
#include <kheap.h>
#include <kprintf.h>
#include <sched.h>
#include <serial.h>
#include <string.h>
#include <thread.h>
#include <proc.h>
#include <tty.h>

static void report(const char *what, uint64_t total, uint32_t n)
{
    kprintf("  %-28s %8lu cycles\n", what, (uint32_t)(total / n));
}

/* --- memory copies ------------------------------------------------------- */
static uint8_t buf_a[4096] __attribute__((aligned(4096)));
static uint8_t buf_b[4096] __attribute__((aligned(4096)));

static void bench_memory(void)
{
    enum { N = 2000 };
    uint64_t t = cycles_now();
    for (int i = 0; i < N; i++)
        memcpy(buf_a, buf_b, sizeof buf_a);
    report("memcpy 4 KiB", cycles_now() - t, N);

    t = cycles_now();
    for (int i = 0; i < N; i++)
        memset(buf_a, i, sizeof buf_a);
    report("memset 4 KiB", cycles_now() - t, N);
}

/* --- kmalloc ------------------------------------------------------------- */
static void bench_kmalloc(void)
{
    enum { N = 2000, LIVE = 1000 };
    static void *live[LIVE];

    uint64_t t = cycles_now();
    for (int i = 0; i < N; i++)
        kfree(kmalloc(64));
    report("kmalloc+kfree 64 B", cycles_now() - t, N);

    /* The same with 1000 other objects alive: does the cost grow? */
    for (int i = 0; i < LIVE; i++)
        live[i] = kmalloc(64);
    t = cycles_now();
    for (int i = 0; i < N; i++)
        kfree(kmalloc(64));
    report("  ... with 1000 objects live", cycles_now() - t, N);
    for (int i = 0; i < LIVE; i++)
        kfree(live[i]);
}

/* --- thread switch ------------------------------------------------------- */
static volatile int stop;

static void yielder(void *arg)
{
    (void)arg;
    while (!stop)
        thread_yield();
}

static void bench_switch(void)
{
    enum { N = 20000 };
    stop = 0;
    struct thread *t = thread_create("yielder", yielder, NULL);
    thread_yield();                         /* let it start */

    uint64_t c = cycles_now();
    for (int i = 0; i < N; i++)
        thread_yield();                     /* main -> yielder -> main */
    report("thread switch", cycles_now() - c, 2 * N);

    stop = 1;
    while (t->state != THREAD_DEAD)
        thread_yield();
}

/* --- user programs ------------------------------------------------------- */
static void run_program(const char *path)
{
    static const char *const argv[] = { "bench", NULL };
    static const char *const envp[] = { NULL };
    struct process *p = proc_spawn(path, argv, envp);
    if (!p) {
        kprintf("  %s: failed to load\n", path);
        return;
    }
    proc_wait_kernel(p);
}

static void bench_exec(void)
{
    enum { N = 200 };
    uint64_t t = cycles_now();
    for (int i = 0; i < N; i++)
        run_program("/bin/true");
    report("exec + exit (ELF 'true')", cycles_now() - t, N);
}

/* --- slot timing --------------------------------------------------------- */
/* "brakes" owns slots 4, 9, 14, 19: it should start every 5 ms exactly.
   We note the cycle counter each time it starts and compare the gaps with
   5 ms. The worst gap is the jitter a real-time task would see. */
enum { SAMPLES = 200 };
static uint64_t starts[SAMPLES];
static volatile int nstarts;

static void brakes(void *arg)
{
    (void)arg;
    while (nstarts < SAMPLES) {
        starts[nstarts++] = cycles_now();
        cpu_wait();                         /* sleep through the rest of the slot */
    }
}

static void bench_slots(void)
{
    thread_create("brakes", brakes, NULL);
    sched_start();
    while (nstarts < SAMPLES)
        cpu_wait();

    uint64_t ideal = 5000ull * cycles_per_us();
    uint64_t worst = 0;
    for (int i = 2; i < SAMPLES; i++) {     /* skip the first, partial frame */
        uint64_t gap = starts[i] - starts[i - 1];
        uint64_t dev = gap > ideal ? gap - ideal : ideal - gap;
        if (dev > worst)
            worst = dev;
    }
    kprintf("  %-28s %8lu us\n", "slot start jitter (worst)",
            (uint32_t)(worst / cycles_per_us()));
}

/* --- disk ------------------------------------------------------------------ */

/* Raw reads from the boot disk (hda), 64 KiB at a time, nothing cached.
   Counted: the CPU's work only (port I/O, interrupts, thread switches),
   not the time it sleeps waiting for the disk — in QEMU that wait jumps
   the virtual clock ahead by however long the Mac took, so it changes
   from run to run. A real disk's speed needs real hardware. */
static void bench_disk(void)
{
    enum { N = 20, SECTORS = 128 };
    static uint8_t buf[SECTORS * SECTOR_SIZE];
    struct blockdev *hda = blockdev_count() ? blockdev_get(0) : NULL;
    if (!hda)
        return;
    uint64_t idle = cpu_idle_cycles(), t = cycles_now();
    for (int i = 0; i < N; i++)
        if (blockdev_read(hda, i * SECTORS, SECTORS, buf) < 0) {
            kprintf("  disk read failed\n");
            return;
        }
    t = (cycles_now() - t - (cpu_idle_cycles() - idle)) / N;
    kprintf("  %-28s %8lu cycles\n", "disk read 64 KiB, CPU work", (uint32_t)t);

    /* The same block again and again: what a block-cache hit costs. */
    enum { HITS = 1000 };
    struct buf *b = bread(hda, 0, 4096);
    if (!b)
        return;
    brelse(b);
    t = cycles_now();
    for (int i = 0; i < HITS; i++)
        brelse(bread(hda, 0, 4096));
    t = (cycles_now() - t) / HITS;
    kprintf("  %-28s %8lu cycles\n", "bread + brelse (cached)", (uint32_t)t);
}

/* --- boot to a command line ----------------------------------------------- */

static uint64_t shell_start, shell_ready;

void bench_input_wait(void)
{
    if (shell_start && !shell_ready)
        shell_ready = cycles_now();
}

/* Start /init the way a normal boot does (it starts bash) and time it
   until the first program waits for a key: bash showing its prompt. That
   is everything a person waits for after the kernel is ready — /init,
   the dynamic linker, bash's libraries, /etc/profile. Returns microseconds. */
static uint32_t bench_shell(void)
{
    static const char *const argv[] = { "/init", NULL };
    static const char *const envp[] = {
        "HOME=/", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "TERM=linux", NULL,
    };
    uint64_t idle0 = cpu_idle_cycles();
    shell_start = cycles_now();
    struct process *p = proc_spawn("/init", argv, envp);
    if (!p)
        return 0;
    proc_set_init(p);
    tty_set_foreground(p->pgid);
    while (!shell_ready)
        cpu_wait();
    /* Waiting (for a disk, when / is on one) is shown apart, as at boot. */
    uint64_t waited = cpu_idle_cycles() - idle0;
    kprintf("  %-28s %8lu us\n", "  ...of which waiting", (uint32_t)(waited / cycles_per_us()));
    return (uint32_t)((shell_ready - shell_start - waited) / cycles_per_us());
}

void run_benchmarks(uint64_t boot_start)
{
    /* From the first line of kernel_main to here: everything the kernel
       does to get ready (the BIOS and boot loader come before). */
    /* Counted: the CPU's work. Time spent waiting for the disks (the boot
       reads the partition tables and mounts /data) is shown apart: in QEMU
       it's the virtual clock jumping by however long the Mac took, which
       changes from run to run; on real hardware it's seek time. */
    uint64_t ready = cycles_now(), waited = cpu_idle_cycles();
    uint32_t ready_us = (uint32_t)((ready - boot_start - waited) / cycles_per_us());
    kprintf("Cycle counter: %lu MHz\n", cycles_per_us());
    kprintf("Boot: kernel ready after %lu us of CPU work (+ %lu us waiting for disks)\n",
            ready_us, (uint32_t)(waited / cycles_per_us()));
    kprintf("Benchmarks (average cycles per operation):\n");
    bench_memory();
    bench_kmalloc();
    bench_switch();
    bench_exec();
    bench_disk();
    run_program("/bin/bench");                          /* prints its own lines */
    bench_slots();                      /* starts the slot scheduler */
    uint32_t shell_us = bench_shell();  /* last: bash stays behind */
    kprintf("  %-28s %8lu us\n", "/init + bash, CPU work", shell_us);
    kprintf("Boot: command line ready after %lu us of CPU work (kernel ready + that)\n",
            ready_us + shell_us);
    kprintf("Benchmarks done\n");
    serial_flush();                 /* everything out before QEMU stops */

    /* Switch QEMU off: writing to this port (QEMU's "isa-debug-exit"
       device, only in `make bench`) ends the emulator. */
    outb(0xF4, 0);
}
