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
#include <bench.h>
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

void run_benchmarks(uint64_t boot_start)
{
    /* From the first line of kernel_main to here: everything the kernel
       does to get ready (the BIOS and boot loader come before). */
    uint64_t ready = cycles_now();
    kprintf("Cycle counter: %lu MHz\n", cycles_per_us());
    kprintf("Boot: kernel ready after %lu us\n",
            (uint32_t)((ready - boot_start) / cycles_per_us()));
    kprintf("Benchmarks (average cycles per operation):\n");
    bench_memory();
    bench_kmalloc();
    bench_switch();
    bench_exec();
    run_program("/bin/bench");                          /* prints its own lines */
    bench_slots();                                      /* last: starts slots */
    kprintf("Benchmarks done\n");
    serial_flush();                 /* everything out before QEMU stops */

    /* Switch QEMU off: writing to this port (QEMU's "isa-debug-exit"
       device, only in `make bench`) ends the emulator. */
    outb(0xF4, 0);
}
