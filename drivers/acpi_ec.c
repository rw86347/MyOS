/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * acpi_ec.c — the embedded controller, for the firmware's AML
 *
 * A laptop has a small microcontroller of its own, the "embedded
 * controller" (EC): it runs the keyboard matrix, the fans, the charger,
 * and it talks to the battery's own chip. The OS reaches it through two
 * I/O ports (ACPI spec, chapter 12), usually 0x62 (data) and 0x66
 * (command and status):
 *
 *   status bits   0 OBF  the EC put a byte in the data port for us
 *                 1 IBF  the EC hasn't taken our last byte yet
 *                 5 SCI_EVT  the EC has news (a "query" waits)
 *   READ  (0x80)  then the address; the EC answers with the byte there
 *   WRITE (0x81)  then the address, then the byte
 *   QUERY (0x84)  the EC answers with the news's number, xx: the OS runs
 *                 the EC device's method _Qxx (it says what happened,
 *                 often with Notify (BAT0, 0x80): "the battery changed")
 *
 * The OS itself never reads the battery here: the DSDT declares an
 * OperationRegion (EmbeddedControl) with named fields (say B1RC, the
 * remaining capacity) at EC addresses, and the battery's _BST reads them.
 * This file is the "address space handler" that turns those field reads
 * into READ commands.
 *
 * Every byte is a handshake, a few microseconds to a millisecond; all of
 * it runs in the acpi thread (acpi_os.c), never in an interrupt. A wait
 * spins for 1 ms, then sleeps a millisecond at a time, and gives up after
 * half a second (a hung EC must not hang the thread).
 *
 * The EC's news raises a GPE (its _GPE says which); the handler only wakes
 * the thread, which sends the queries. The thread also looks every time it
 * wakes, so news isn't lost if the SCI isn't wired.
 */
#include <acpi.h>
#include <cycles.h>
#include <interrupts.h>
#include <io.h>
#include <kprintf.h>
#include <thread.h>
#include <timer.h>

#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define EC_OBF      0x01
#define EC_IBF      0x02
#define EC_SCI_EVT  0x20
#define EC_READ     0x80
#define EC_WRITE    0x81
#define EC_QUERY    0x84
#define QUERIES     32                  /* most news handled per wake-up */

static uint16_t ec_data, ec_cmd;
static uacpi_namespace_node *ec_node;
static int ec_glk, ec_ready;
static uint32_t ec_timeouts;

/* Wait until (status & mask) == want: 0, or -1 after half a second. */
static int ec_wait(uint8_t mask, uint8_t want)
{
    uint64_t spin_end = cycles_now() + 1000ull * cycles_per_us();
    while (cycles_now() < spin_end) {
        if ((inb(ec_cmd) & mask) == want)
            return 0;
        __asm__ volatile ("pause");
    }
    struct wait_queue q = { NULL };
    uint32_t end = timer_ms() + 500;
    while ((int32_t)(timer_ms() - end) < 0) {
        if ((inb(ec_cmd) & mask) == want)
            return 0;
        uint32_t f = (uint32_t)irq_save();
        wq_sleep_until(&q, timer_ms() + 2);
        irq_restore(f);
    }
    if (ec_timeouts++ < 5)
        klog("acpi-ec: no answer (status %x)\n", inb(ec_cmd));
    return -1;
}

static int ec_command(uint8_t cmd)
{
    if (ec_wait(EC_IBF, 0) < 0)
        return -1;
    outb(ec_cmd, cmd);
    return 0;
}

static int ec_put(uint8_t b)
{
    if (ec_wait(EC_IBF, 0) < 0)
        return -1;
    outb(ec_data, b);
    return 0;
}

static int ec_get(uint8_t *b)
{
    if (ec_wait(EC_OBF, EC_OBF) < 0)
        return -1;
    *b = inb(ec_data);
    return 0;
}

/* The firmware may use the EC too (from SMM): if the EC's _GLK says so,
   each transaction holds ACPI's global lock. */
static uint32_t lock(void)
{
    uint32_t seq = 0;
    if (ec_glk)
        uacpi_acquire_global_lock(0xFFFF, &seq);
    return seq;
}

static void unlock(uint32_t seq)
{
    if (ec_glk)
        uacpi_release_global_lock(seq);
}

static int ec_read(uint8_t addr, uint8_t *v)
{
    uint32_t seq = lock();
    int r = ec_command(EC_READ) < 0 || ec_put(addr) < 0 || ec_get(v) < 0 ? -1 : 0;
    unlock(seq);
    return r;
}

static int ec_write(uint8_t addr, uint8_t v)
{
    uint32_t seq = lock();
    int r = ec_command(EC_WRITE) < 0 || ec_put(addr) < 0 || ec_put(v) < 0 ||
            ec_wait(EC_IBF, 0) < 0 ? -1 : 0;
    unlock(seq);
    return r;
}

/* A field of 1 to 8 bytes: one READ or WRITE per byte, low byte first. */
static uacpi_status ec_region(uacpi_region_op op, uacpi_handle data)
{
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE)
        return UACPI_STATUS_OK;         /* attach, detach: nothing to set up */
    uacpi_region_rw_data *rw = data;
    if (rw->offset + rw->byte_width > 256)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t v = op == UACPI_REGION_OP_WRITE ? rw->value : 0;
    for (int i = 0; i < rw->byte_width; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        uint8_t addr = (uint8_t)(rw->offset + i);
        if (op == UACPI_REGION_OP_READ) {
            if (ec_read(addr, &b) < 0)
                return UACPI_STATUS_HARDWARE_TIMEOUT;
            v |= (uint64_t)b << (8 * i);
        } else if (ec_write(addr, b) < 0) {
            return UACPI_STATUS_HARDWARE_TIMEOUT;
        }
    }
    if (op == UACPI_REGION_OP_READ)
        rw->value = v;
    return UACPI_STATUS_OK;
}

/* In the interrupt: the EC has news. The thread asks what (acpi_ec_poll). */
static uacpi_interrupt_ret ec_gpe(uacpi_handle ctx, uacpi_namespace_node *dev, uacpi_u16 idx)
{
    (void)ctx, (void)dev, (void)idx;
    acpi_poke();
    return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
}

void acpi_ec_poll(void)
{
    if (!ec_ready)
        return;
    for (int n = 0; n < QUERIES && (inb(ec_cmd) & EC_SCI_EVT); n++) {
        uint8_t q = 0;
        uint32_t seq = lock();
        int r = ec_command(EC_QUERY) < 0 || ec_get(&q) < 0 ? -1 : 0;
        unlock(seq);
        if (r < 0 || !q)
            return;                     /* 0: nothing after all */
        char name[5] = { '_', 'Q', "0123456789ABCDEF"[q >> 4], "0123456789ABCDEF"[q & 15], 0 };
        uacpi_execute(ec_node, name, NULL);     /* (a missing _Qxx: news nobody wanted) */
    }
}

/* The EC's two ports, from its _CRS: data first, then command. */
static uacpi_iteration_decision crs_port(void *user, uacpi_resource *r)
{
    int *n = user;
    uint16_t port;
    if (r->type == UACPI_RESOURCE_TYPE_IO)
        port = r->io.minimum;
    else if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO)
        port = r->fixed_io.address;
    else
        return UACPI_ITERATION_DECISION_CONTINUE;
    if ((*n)++ == 0)
        ec_data = port;
    else
        ec_cmd = port;
    return *n == 2 ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_ec(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user, (void)depth;
    int n = 0;
    uacpi_for_each_device_resource(node, "_CRS", crs_port, &n);
    if (n < 2)
        return UACPI_ITERATION_DECISION_CONTINUE;
    ec_node = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

void acpi_ec_init(void)
{
    /* The ECDT (if the firmware has one) names the EC and its ports. */
    uacpi_table t;
    if (uacpi_table_find_by_signature("ECDT", &t) == UACPI_STATUS_OK) {
        struct acpi_ecdt *e = t.ptr;
        if (e->hdr.length > sizeof *e && e->ec_control.address && e->ec_data.address &&
            uacpi_namespace_node_find(NULL, e->ec_id, &ec_node) == UACPI_STATUS_OK) {
            ec_cmd = (uint16_t)e->ec_control.address;
            ec_data = (uint16_t)e->ec_data.address;
        }
        uacpi_table_unref(&t);
    }
    if (!ec_node)
        uacpi_find_devices("PNP0C09", found_ec, NULL);
    if (!ec_node)
        return;                         /* a desktop, or QEMU: no EC */
    uint64_t v;
    ec_glk = uacpi_eval_simple_integer(ec_node, "_GLK", &v) == UACPI_STATUS_OK && v;
    if (uacpi_install_address_space_handler(ec_node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                                            ec_region, NULL) != UACPI_STATUS_OK) {
        klog("acpi-ec: could not take its address space\n");
        return;
    }
    ec_ready = 1;
    int gpe = -1;
    if (uacpi_eval_simple_integer(ec_node, "_GPE", &v) == UACPI_STATUS_OK && v < 256 &&
        uacpi_install_gpe_handler(NULL, (uint16_t)v, UACPI_GPE_TRIGGERING_EDGE, ec_gpe, NULL)
            == UACPI_STATUS_OK)
        gpe = (int)v;
    klog("acpi-ec: ports %x/%x, GPE %d%s\n", ec_data, ec_cmd, gpe,
         ec_glk ? ", with the global lock" : "");
}
