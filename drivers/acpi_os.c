/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * acpi_os.c — the ACPI interpreter, and what it needs from the kernel
 *
 * The firmware describes the parts of a laptop that have no standard
 * registers (the battery, the charger, the lid, the fans) in AML, a
 * bytecode in its DSDT and SSDT tables (acpi.c finds them). To ask "how
 * full is the battery?" the OS runs the battery's _BST method. uACPI
 * (third_party/uacpi, MIT licence) is the interpreter: 24,000 lines, a
 * quarter of Linux's ACPICA, written to be ported. A port gives it the
 * uacpi_kernel_* functions below: map physical memory, PCI config space,
 * I/O ports, memory, time, locks, and "run this later in a thread".
 *
 * All AML runs in one kernel thread, "acpi" (acpi_start). Loading the
 * tables takes tens of milliseconds and a method may wait for slow
 * hardware (the embedded controller, acpi_ec.c), so none of it may run in
 * an interrupt or a system call: the battery's numbers are read here and
 * kept, and /sys only shows the copy (battery.c).
 *
 * The SCI (System Control Interrupt, IRQ 9 on a PC) is how the firmware
 * says something happened: the power button, or a "general purpose event"
 * (GPE) such as the embedded controller having news. It is level-
 * triggered: the line stays up until the OS clears the event's status bit.
 * uACPI's handler does that, in the interrupt; the methods the event asks
 * for (_Lxx, _Qxx, Notify) are queued as work for the thread. Should the
 * line stay up anyway (a board this doesn't understand), the IRQ is masked
 * after 10,000 unanswered interrupts in a second rather than hang the
 * machine; the battery is then still read every few seconds.
 *
 * uACPI unmaps what it maps; the kernel's window of device mappings is
 * never given back (pci.c), so mappings are kept and reused: the tables
 * (acpi.c made those, cached), and a few device pages (uncached).
 */
#include <acpi.h>
#include <cycles.h>
#include <interrupts.h>
#include <io.h>
#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pci.h>
#include <pic.h>
#include <pmm.h>
#include <string.h>
#include <thread.h>
#include <timer.h>

#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/uacpi.h>

/* --- memory ---------------------------------------------------------------- */

#define DEV_MAPS 32
static struct { uint64_t page; uint32_t bytes; uintptr_t va; } dev_maps[DEV_MAPS];
static int ndev_maps;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out)
{
    *out = acpi_rsdp();
    return *out ? UACPI_STATUS_OK : UACPI_STATUS_NOT_FOUND;
}

/* Low RAM is in the direct map; the tables are mapped already (acpi.c);
   anything else is device memory (an OperationRegion over a chipset's
   registers), mapped uncached, once per page range. */
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    if (!len)
        len = 1;
    if (addr + len <= pmm_end())
        return P2V(addr);
    const void *t = acpi_mapped(addr, (uint32_t)len);
    if (t)
        return (void *)t;
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint32_t bytes = (uint32_t)((addr - page + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
    uint32_t f = (uint32_t)irq_save();
    for (int i = 0; i < ndev_maps; i++)
        if (page >= dev_maps[i].page && page - dev_maps[i].page + bytes <= dev_maps[i].bytes) {
            uintptr_t va = dev_maps[i].va + (uintptr_t)(addr - dev_maps[i].page);
            irq_restore(f);
            return (void *)va;
        }
    irq_restore(f);
    volatile void *v = pci_map_mmio(page, bytes);
    if (!v)
        return UACPI_MAP_FAILED;
    f = (uint32_t)irq_save();
    if (ndev_maps < DEV_MAPS) {
        dev_maps[ndev_maps].page = page;
        dev_maps[ndev_maps].bytes = bytes;
        dev_maps[ndev_maps++].va = (uintptr_t)v;
    } else {
        klog("acpi: more than %d device mappings: the window fills\n", DEV_MAPS);
    }
    irq_restore(f);
    return (uint8_t *)v + (addr - page);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len)
{
    (void)addr, (void)len;              /* kept for the next time */
}

void *uacpi_kernel_alloc(uacpi_size size)
{
    return kmalloc(size ? size : 1);
}

void uacpi_kernel_free(void *mem)
{
    kfree(mem);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg)
{
    klog("acpi%s: %s", level <= UACPI_LOG_ERROR ? " error" :
                       level == UACPI_LOG_WARN ? " warning" : "", msg);
}

/* --- PCI config space and I/O ports ---------------------------------------- */

/* A handle is the address itself, never NULL (bit 24 set). */
#define PCI_HANDLE(b, d, f) ((uacpi_handle)(uintptr_t)(1u << 24 | (b) << 16 | (d) << 8 | (f)))
#define PCI_BUS(h)          ((uint8_t)((uintptr_t)(h) >> 16))
#define PCI_SLOT(h)         ((uint8_t)((uintptr_t)(h) >> 8))
#define PCI_FUNC(h)         ((uint8_t)(uintptr_t)(h))

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out)
{
    if (a.segment || a.device > 31 || a.function > 7)
        return UACPI_STATUS_UNIMPLEMENTED;  /* one segment, by port 0xCF8 */
    *out = PCI_HANDLE(a.bus, a.device, a.function);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle h)
{
    (void)h;
}

#define PCI_RW(bits, width)                                                              \
uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle h, uacpi_size off, uacpi_u##bits *v) \
{                                                                                        \
    *v = (uacpi_u##bits)pci_cfg_read(PCI_BUS(h), PCI_SLOT(h), PCI_FUNC(h), (uint16_t)off, width); \
    return UACPI_STATUS_OK;                                                              \
}                                                                                        \
uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle h, uacpi_size off, uacpi_u##bits v) \
{                                                                                        \
    pci_cfg_write(PCI_BUS(h), PCI_SLOT(h), PCI_FUNC(h), (uint16_t)off, width, v);        \
    return UACPI_STATUS_OK;                                                              \
}
PCI_RW(8, 1)
PCI_RW(16, 2)
PCI_RW(32, 4)

/* An I/O range's handle is its first port, with bit 16 set (never NULL). */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out)
{
    if (base > 0xFFFF || len > 0x10000 - base)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out = (uacpi_handle)(uintptr_t)(0x10000u | base);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle h)
{
    (void)h;
}

#define PORT(h, off) ((uint16_t)((uintptr_t)(h) + (off)))

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v)
{ *v = inb(PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v)
{ *v = inw(PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v)
{ *v = inl(PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v)
{ outb(PORT(h, off), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v)
{ outw(PORT(h, off), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v)
{ outl(PORT(h, off), v); return UACPI_STATUS_OK; }

/* --- time ------------------------------------------------------------------- */

/* From the cycle counter, split so the multiply can't overflow. */
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    uint64_t c = cycles_now(), mhz = cycles_per_us();
    return c / mhz * 1000 + c % mhz * 1000 / mhz;
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
    uint64_t end = cycles_now() + (uint64_t)usec * cycles_per_us();
    while (cycles_now() < end)
        __asm__ volatile ("pause");
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    struct wait_queue q = { NULL };
    uint32_t f = (uint32_t)irq_save();
    uint32_t end = timer_ms() + (uint32_t)msec + 1;    /* + the tick already begun */
    while ((int32_t)(timer_ms() - end) < 0)
        wq_sleep_until(&q, end ? end : 1);
    irq_restore(f);
}

/* --- locks and events -------------------------------------------------------- */

/* uACPI makes these recursive itself; to it a timeout is milliseconds,
   0 = only try, 0xFFFF = forever. Sleepers wait on a queue, interrupts off
   between the test and the sleep, so no wake-up is lost. */
struct amutex { struct thread *owner; struct wait_queue q; };
struct aevent { uint32_t count; struct wait_queue q; };

/* Wait on q while *busy, at most timeout ms. 1: still busy (timed out). */
static int wait_while(struct wait_queue *q, const volatile void *busy, int is_count,
                      uint16_t timeout)
{
    uint32_t end = timer_ms() + timeout + 1;
    for (;;) {
        int b = is_count ? !*(const volatile uint32_t *)busy
                         : *(struct thread *const volatile *)busy != NULL;
        if (!b)
            return 0;
        if (!timeout || (timeout != 0xFFFF && (int32_t)(timer_ms() - end) >= 0))
            return 1;
        wq_sleep_until(q, timeout == 0xFFFF ? 0 : end ? end : 1);
    }
}

uacpi_handle uacpi_kernel_create_mutex(void)
{
    struct amutex *m = kmalloc(sizeof *m);
    if (m)
        memset(m, 0, sizeof *m);
    return m;
}

void uacpi_kernel_free_mutex(uacpi_handle h)
{
    kfree(h);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout)
{
    struct amutex *m = h;
    uint32_t f = (uint32_t)irq_save();
    int busy = wait_while(&m->q, &m->owner, 0, timeout);
    if (!busy)
        m->owner = thread_current();
    irq_restore(f);
    return busy ? UACPI_STATUS_TIMEOUT : UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle h)
{
    struct amutex *m = h;
    uint32_t f = (uint32_t)irq_save();
    m->owner = NULL;
    wq_wake_all(&m->q);
    irq_restore(f);
}

uacpi_handle uacpi_kernel_create_event(void)
{
    struct aevent *e = kmalloc(sizeof *e);
    if (e)
        memset(e, 0, sizeof *e);
    return e;
}

void uacpi_kernel_free_event(uacpi_handle h)
{
    kfree(h);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout)
{
    struct aevent *e = h;
    uint32_t f = (uint32_t)irq_save();
    int none = wait_while(&e->q, &e->count, 1, timeout);
    if (!none)
        e->count--;
    irq_restore(f);
    return !none;
}

void uacpi_kernel_signal_event(uacpi_handle h)        /* also from the SCI */
{
    struct aevent *e = h;
    uint32_t f = (uint32_t)irq_save();
    e->count++;
    wq_wake_all(&e->q);
    irq_restore(f);
}

void uacpi_kernel_reset_event(uacpi_handle h)
{
    ((struct aevent *)h)->count = 0;
}

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    return thread_current();
}

/* One CPU: a spinlock is "interrupts off". */
static char a_spinlock;

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    return &a_spinlock;
}

void uacpi_kernel_free_spinlock(uacpi_handle h)
{
    (void)h;
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h)
{
    (void)h;
    return irq_save();
}

void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f)
{
    (void)h;
    irq_restore(f);
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    return irq_save();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state f)
{
    irq_restore(f);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r)
{
    if (r->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        klog("acpi: the firmware reports a fatal error (type %u, code %u, arg %u)\n",
             (unsigned)r->fatal.type, (unsigned)r->fatal.code, (unsigned)r->fatal.arg);
    return UACPI_STATUS_OK;             /* a breakpoint: nothing to stop in */
}

/* --- the SCI ------------------------------------------------------------------ */

#define STORM 10000                     /* unanswered SCIs in a second: give up on it */

static uacpi_interrupt_handler sci_fn;
static uacpi_handle sci_ctx;
static int sci_irq = -1;
static uint32_t sci_second, sci_unanswered;

static void sci_isr(struct regs *r)
{
    (void)r;
    if (sci_fn(sci_ctx) == UACPI_INTERRUPT_HANDLED)
        return;
    uint32_t s = timer_ms() / 1000;
    if (s != sci_second)
        sci_second = s, sci_unanswered = 0;
    if (++sci_unanswered == STORM) {
        pic_mask(sci_irq);
        klog("acpi: IRQ %d keeps firing with no event: masked (the battery is polled)\n",
             sci_irq);
    }
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler fn,
                                                    uacpi_handle ctx, uacpi_handle *out)
{
    /* The SCI only (a GPE block with its own line is rare). In 8259 mode
       it is an ISA IRQ, level-triggered (the PIC's ELCR says so). */
    if (sci_fn || irq < 3 || irq > 15 || irq == 8 || irq == 13) {
        klog("acpi: IRQ %u for the firmware's events: not wired (they are polled)\n",
             (unsigned)irq);
        *out = &sci_fn;
        return UACPI_STATUS_OK;
    }
    sci_ctx = ctx;
    sci_irq = (int)irq;
    sci_fn = fn;
    pic_set_level((int)irq);
    irq_install((int)irq, sci_isr);
    *out = &sci_irq;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn, uacpi_handle h)
{
    (void)fn;
    if (h == &sci_irq && sci_irq >= 0)
        pic_mask(sci_irq);
    return UACPI_STATUS_OK;
}

/* --- work for the thread ------------------------------------------------------ */

/* A ring, not a list: GPE work is queued in the interrupt, which must not
   allocate. 64 is far more than a board raises at once. */
#define WORK_RING 64

static struct { uacpi_work_handler fn; uacpi_handle ctx; } ring[WORK_RING];
static uint32_t ring_head, ring_tail;   /* head: next to run; tail: next free */
static struct thread *acpi_thread;
static struct wait_queue acpi_wq;       /* the thread waits here for work */
static struct wait_queue idle_wq;       /* wait_for_work_completion waits here */
static volatile int busy, poke;

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler fn,
                                        uacpi_handle ctx)
{
    (void)type;
    uint32_t f = (uint32_t)irq_save();
    if (ring_tail - ring_head == WORK_RING) {
        irq_restore(f);
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    ring[ring_tail % WORK_RING].fn = fn;
    ring[ring_tail % WORK_RING].ctx = ctx;
    ring_tail++;
    wq_wake_all(&acpi_wq);
    irq_restore(f);
    return UACPI_STATUS_OK;
}

/* Run what is queued. In the thread only. */
static void run_work(void)
{
    for (;;) {
        uint32_t f = (uint32_t)irq_save();
        if (ring_head == ring_tail) {
            busy = 0;
            wq_wake_all(&idle_wq);
            irq_restore(f);
            return;
        }
        uacpi_work_handler fn = ring[ring_head % WORK_RING].fn;
        uacpi_handle ctx = ring[ring_head % WORK_RING].ctx;
        ring_head++;
        busy = 1;
        irq_restore(f);
        fn(ctx);
    }
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    if (thread_current() == acpi_thread) {
        run_work();                     /* waiting for ourselves would never end */
        return UACPI_STATUS_OK;
    }
    uint32_t f = (uint32_t)irq_save();
    while (busy || ring_head != ring_tail)
        wq_sleep(&idle_wq);
    irq_restore(f);
    return UACPI_STATUS_OK;
}

void acpi_poke(void)                    /* also from interrupts */
{
    uint32_t f = (uint32_t)irq_save();
    poke = 1;
    wq_wake_all(&acpi_wq);
    irq_restore(f);
}

/* --- the thread ---------------------------------------------------------------- */

static uacpi_interrupt_ret power_button(uacpi_handle ctx)
{
    (void)ctx;
    klog("acpi: the power button\n");   /* (nothing listens yet) */
    return UACPI_INTERRUPT_HANDLED;
}

static int step(const char *what, uacpi_status st)
{
    if (st == UACPI_STATUS_OK)
        return 0;
    klog("acpi: %s failed: %s\n", what, uacpi_status_to_string(st));
    return -1;
}

static void acpi_main(void *arg)
{
    (void)arg;
    uint32_t t0 = timer_ms();
    if (step("starting the interpreter", uacpi_initialize(0)) ||
        step("loading the tables' AML", uacpi_namespace_load()))
        thread_exit();
    acpi_ec_init();                     /* before _INI and _REG: their AML may read it */
    if (step("initializing the devices", uacpi_namespace_initialize()))
        thread_exit();
    step("enabling events", uacpi_finalize_gpe_initialization());
    uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button, NULL);
    klog("acpi: the AML is running (%u ms)\n", (unsigned)(timer_ms() - t0));
    battery_init();
    backlight_init();

    for (;;) {
        run_work();
        acpi_ec_poll();                 /* the embedded controller's news, if any */
        backlight_poll();               /* a brightness someone wrote */
        uint32_t ms = battery_poll();   /* when it wants to look again (0: never) */
        uint32_t end = ms ? timer_ms() + ms : 0;
        uint32_t f = (uint32_t)irq_save();
        if (!poke && ring_head == ring_tail)
            wq_sleep_until(&acpi_wq, ms && !end ? 1 : end);
        poke = 0;
        irq_restore(f);
    }
}

void acpi_start(void)
{
    if (!acpi_rsdp())
        return;                         /* no ACPI at all (an old PC, QEMU -no-acpi) */
    acpi_thread = thread_create("acpi", acpi_main, NULL);
    if (!acpi_thread)
        klog("acpi: no thread for the interpreter\n");
}
