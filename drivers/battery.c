/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * battery.c — the battery and the charger, from ACPI, as Linux shows them
 *
 * ACPI describes a battery as a device with the id PNP0C0A and three
 * methods (ACPI spec 10.2):
 *
 *   _BIX (or the older _BIF)  what it is: the unit (mWh or mAh), how much
 *         it held new ("design capacity"), how much it holds now when full
 *         (lower as it ages: full / design is its health), its voltage,
 *         how many charge cycles it has done, its model and maker
 *   _BST  how it is now: charging or discharging, the rate (mW or mA),
 *         what is left, the voltage
 *   _STA  bit 4: is a battery in the slot at all
 *
 * and a charger ("AC adapter", ACPI0003) with _PSR: 1 = plugged in.
 * The methods read the embedded controller (acpi_ec.c), so they are run
 * in the acpi thread, every 10 seconds and whenever the firmware says
 * something changed (Notify 0x80: the state; 0x81: the information, a
 * battery put in). The numbers are kept here; reading
 *
 *   /sys/class/power_supply/BAT0/{status, capacity, energy_now, ...}
 *   /sys/class/power_supply/AC/online
 *
 * shows the copy (O(1): a system call never waits for the EC). The files
 * and units are Linux's (µWh, µW, µV; a battery that counts in mAh has
 * charge_* and current_now instead of energy_* and power_now), so the
 * tray, and upower or a shell loop, read them as on Linux.
 */
#include <acpi.h>
#include <errno.h>
#include <interrupts.h>
#include <kprintf.h>
#include <string.h>
#include <timer.h>
#include <vfs.h>

#include <uacpi/notify.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define MAX_SUPPLIES 4
#define POLL_MS      10000
#define UNKNOWN      0xFFFFFFFFu        /* ACPI's "I can't tell" */

enum { BATTERY, MAINS };

struct ps_state {                       /* what /sys shows: a copy, swapped in whole */
    uint8_t present, unit_mah, rechargeable, online;
    uint32_t design, full, design_mv, cycles;       /* _BIX */
    uint32_t state, rate, left, mv;                 /* _BST */
    char model[24], serial[16], maker[24], chem[8];
};

static struct supply {
    uacpi_namespace_node *node;
    char name[5];
    uint8_t kind, shown, info_ok;
    volatile uint8_t dirty;             /* 1: state changed; 2: information too */
    struct ps_state s;
} supplies[MAX_SUPPLIES];
static int nsupplies;
static uint32_t last_poll;

/* --- reading the methods (the acpi thread) ------------------------------------ */

static uint32_t pkg_int(uacpi_object_array *a, uacpi_size i)
{
    uint64_t v;
    if (i >= a->count || uacpi_object_get_integer(a->objects[i], &v) != UACPI_STATUS_OK)
        return UNKNOWN;
    return (uint32_t)v;
}

static void pkg_str(uacpi_object_array *a, uacpi_size i, char *out, uint32_t size)
{
    uacpi_data_view d;
    out[0] = 0;
    if (i >= a->count || uacpi_object_get_string_or_buffer(a->objects[i], &d) != UACPI_STATUS_OK)
        return;
    uint32_t n = 0;
    for (uacpi_size k = 0; k < d.length && n + 1 < size; k++) {
        char c = (char)d.const_bytes[k];
        if (!c)
            break;
        out[n++] = c >= ' ' && c < 127 ? c : '?';     /* firmware text, shown as is */
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

/* _BIX, or _BIF on older firmware: the same facts, _BIX has two more
   leading fields and the cycle count. */
static void read_info(struct supply *p, struct ps_state *s)
{
    uacpi_object *o = NULL;
    uacpi_object_array a;
    int bix = uacpi_eval_typed(p->node, "_BIX", NULL, UACPI_OBJECT_PACKAGE_BIT, &o)
              == UACPI_STATUS_OK && uacpi_object_get_package(o, &a) == UACPI_STATUS_OK &&
              a.count >= 20;
    if (!bix) {
        uacpi_object_unref(o);
        o = NULL;
        if (uacpi_eval_typed(p->node, "_BIF", NULL, UACPI_OBJECT_PACKAGE_BIT, &o)
                != UACPI_STATUS_OK || uacpi_object_get_package(o, &a) != UACPI_STATUS_OK ||
            a.count < 13) {
            uacpi_object_unref(o);
            return;
        }
    }
    uacpi_size b = bix ? 1 : 0;         /* _BIX: [0] is its revision */
    s->unit_mah = pkg_int(&a, b + 0) == 1;
    s->design = pkg_int(&a, b + 1);
    s->full = pkg_int(&a, b + 2);
    s->rechargeable = pkg_int(&a, b + 3) == 1;
    s->design_mv = pkg_int(&a, b + 4);
    s->cycles = bix ? pkg_int(&a, 8) : UNKNOWN;
    uacpi_size m = bix ? 16 : 9;        /* model, serial, type, maker */
    pkg_str(&a, m, s->model, sizeof s->model);
    pkg_str(&a, m + 1, s->serial, sizeof s->serial);
    pkg_str(&a, m + 2, s->chem, sizeof s->chem);
    pkg_str(&a, m + 3, s->maker, sizeof s->maker);
    uacpi_object_unref(o);
    p->info_ok = 1;
}

static void read_state(struct supply *p, struct ps_state *s)
{
    uacpi_object *o = NULL;
    uacpi_object_array a;
    if (uacpi_eval_typed(p->node, "_BST", NULL, UACPI_OBJECT_PACKAGE_BIT, &o)
            == UACPI_STATUS_OK && uacpi_object_get_package(o, &a) == UACPI_STATUS_OK &&
        a.count >= 4) {
        s->state = pkg_int(&a, 0);
        s->rate = pkg_int(&a, 1);
        s->left = pkg_int(&a, 2);
        s->mv = pkg_int(&a, 3);
        /* A signed rate (some firmware): the size of it is the rate. */
        if (s->rate != UNKNOWN && (s->rate & 0x80000000u))
            s->rate = (uint32_t)-(int32_t)s->rate;
    }
    uacpi_object_unref(o);
}

static void update(struct supply *p)
{
    struct ps_state s;
    uint32_t f = (uint32_t)irq_save();
    s = p->s;
    uint8_t dirty = p->dirty;
    p->dirty = 0;
    irq_restore(f);
    uint64_t v;
    if (p->kind == MAINS) {
        s.online = uacpi_eval_simple_integer(p->node, "_PSR", &v) == UACPI_STATUS_OK && v;
        s.present = 1;
    } else {
        uint32_t sta = 0x1F;
        uacpi_eval_sta(p->node, &sta);
        s.present = (sta & 0x10) != 0;
        if (s.present && (!p->info_ok || dirty == 2))
            read_info(p, &s);
        if (s.present)
            read_state(p, &s);
        else
            p->info_ok = 0;
    }
    f = (uint32_t)irq_save();
    p->s = s;
    irq_restore(f);
    if (!p->shown && (p->kind == MAINS || p->info_ok)) {
        p->shown = 1;                   /* its files depend on its unit: known now */
        sysfs_power_supply_add((int)(p - supplies));
        if (p->kind == BATTERY)
            klog("battery %s: %s %s, %u %s new, %u now\n", p->name, s.maker, s.model,
                 (unsigned)s.design, s.unit_mah ? "mAh" : "mWh", (unsigned)s.full);
    }
}

static uacpi_status notified(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)node;
    struct supply *p = ctx;
    p->dirty = value == 0x81 ? 2 : p->dirty ? p->dirty : 1;
    update(p);                          /* (we are in the thread: work runs there) */
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found(void *kind, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    if (nsupplies == MAX_SUPPLIES)
        return UACPI_ITERATION_DECISION_BREAK;
    struct supply *p = &supplies[nsupplies++];
    p->node = node;
    p->kind = (uint8_t)(uintptr_t)kind;
    /* Linux's name: the device's own, its padding '_'s dropped ("AC__" is AC). */
    uacpi_object_name n = uacpi_namespace_node_name(node);
    memcpy(p->name, n.text, 4);
    for (int i = 3; i > 1 && p->name[i] == '_'; i--)
        p->name[i] = 0;
    uacpi_install_notify_handler(node, notified, p);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void battery_init(void)
{
    uacpi_find_devices("ACPI0003", found, (void *)(uintptr_t)MAINS);
    uacpi_find_devices("PNP0C0A", found, (void *)(uintptr_t)BATTERY);
    for (int i = 0; i < nsupplies; i++)
        update(&supplies[i]);
    last_poll = timer_ms();
}

uint32_t battery_poll(void)
{
    if (!nsupplies)
        return 0;                       /* nothing to watch: wake only for work */
    uint32_t now = timer_ms();
    int due = now - last_poll >= POLL_MS;
    for (int i = 0; i < nsupplies; i++)
        if (due || supplies[i].dirty)
            update(&supplies[i]);
    if (due)
        last_poll = now;
    return POLL_MS - (timer_ms() - last_poll);
}

/* --- what /sys shows (any thread: from the copy) -------------------------------- */

enum {
    A_TYPE, A_STATUS, A_PRESENT, A_ONLINE, A_TECH, A_CYCLES, A_VMIN, A_VNOW,
    A_RATE, A_DESIGN, A_FULL, A_NOW, A_CAPACITY, A_LEVEL, A_MODEL, A_MAKER, A_SERIAL,
    A_ATTRS
};

static const char *const bat_names[A_ATTRS] = {
    "type", "status", "present", NULL, "technology", "cycle_count",
    "voltage_min_design", "voltage_now", "power_now", "energy_full_design",
    "energy_full", "energy_now", "capacity", "capacity_level", "model_name",
    "manufacturer", "serial_number",
};

int power_supply_count(void)
{
    return nsupplies;
}

const char *power_supply_name(int i)
{
    return supplies[i].shown ? supplies[i].name : NULL;    /* not until its unit is known */
}

int power_supply_attrs(void)
{
    return A_ATTRS;
}

const char *power_supply_attr(int i, int k)
{
    const struct supply *p = &supplies[i];
    if (p->kind == MAINS)
        return k == A_TYPE ? "type" : k == A_ONLINE ? "online" : NULL;
    if (p->s.unit_mah) {                /* the same numbers counted in charge */
        static const char *const mah[] = {
            [A_RATE] = "current_now", [A_DESIGN] = "charge_full_design",
            [A_FULL] = "charge_full", [A_NOW] = "charge_now",
        };
        if (k >= A_RATE && k <= A_NOW)
            return mah[k];
    }
    return bat_names[k];
}

static char *put_num(char *p, uint64_t v)
{
    char b[20];
    int n = 0;
    do
        b[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *p++ = b[--n];
    return p;
}

/* Attribute k's text ("Discharging\n"), or -errno: -ENODEV for a number
   the firmware doesn't know, as Linux does. */
int power_supply_show(int i, int k, char *out, int size)
{
    const struct supply *p = &supplies[i];
    struct ps_state s;
    uint32_t f = (uint32_t)irq_save();
    s = p->s;
    irq_restore(f);
    const char *text = NULL;
    uint64_t num = 0;
    switch (k) {
    case A_TYPE:    text = p->kind == MAINS ? "Mains" : "Battery"; break;
    case A_ONLINE:  num = s.online; break;
    case A_PRESENT: num = s.present; break;
    case A_STATUS:
        if (!s.present)
            text = "Unknown";
        else if (s.state & 2)
            text = "Charging";
        else if (s.state & 1)
            text = "Discharging";
        else                            /* neither: full, or held back by the charger */
            text = s.full != UNKNOWN && s.left != UNKNOWN && s.left >= s.full / 100 * 95
                   ? "Full" : "Not charging";
        break;
    case A_TECH:
        text = !s.rechargeable ? "Unknown" : !strcmp(s.chem, "LION") || !strcmp(s.chem, "Li-ion")
               ? "Li-ion" : !strncmp(s.chem, "LiP", 3) ? "Li-poly" : "Unknown";
        break;
    case A_CYCLES:  num = s.cycles; break;
    case A_VMIN:    num = s.design_mv; break;
    case A_VNOW:    num = s.mv; break;
    case A_RATE:    num = s.rate; break;
    case A_DESIGN:  num = s.design; break;
    case A_FULL:    num = s.full; break;
    case A_NOW:     num = s.left; break;
    case A_CAPACITY:
        if (s.full == UNKNOWN || !s.full || s.left == UNKNOWN)
            return -ENODEV;
        num = s.left >= s.full ? 100 : (uint64_t)s.left * 100 / s.full;
        break;
    case A_LEVEL:
        text = s.state & 4 ? "Critical" :
               s.full != UNKNOWN && s.left != UNKNOWN && s.left >= s.full ? "Full" : "Normal";
        break;
    case A_MODEL:   text = s.model; break;
    case A_MAKER:   text = s.maker; break;
    case A_SERIAL:  text = s.serial; break;
    default:        return -ENODEV;
    }
    if (!text) {
        if (num == UNKNOWN && k != A_ONLINE && k != A_PRESENT)
            return -ENODEV;
        if (k >= A_VMIN && k <= A_NOW)
            num *= 1000;                /* mV, mW, mWh, mA, mAh: Linux counts in micro */
    }
    char buf[32], *e = buf;
    if (text) {
        uint32_t n = (uint32_t)strlen(text);
        if (n > sizeof buf - 2)
            n = sizeof buf - 2;
        memcpy(buf, text, n);
        e = buf + n;
    } else {
        e = put_num(buf, num);
    }
    *e++ = '\n';
    int len = (int)(e - buf);
    if (len > size)
        len = size;
    memcpy(out, buf, (uint32_t)len);
    return len;
}
