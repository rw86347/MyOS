/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * backlight.c — the laptop panel's brightness, through the firmware
 *
 * ACPI describes the built-in panel as a device under the graphics card
 * with three methods (ACPI spec, appendix B, "Video Extensions"):
 *
 *   _BCL  the levels it takes, in percent: the first two are its defaults
 *         (on the charger, on battery), the rest the levels themselves,
 *         in any order, sometimes one twice
 *   _BCM  set one: _BCM (40)
 *   _BQC  which is set now (not on every laptop)
 *
 * As Linux's acpi_video, the levels are sorted and numbered from 0, and
 *
 *   /sys/class/backlight/acpi_video0/brightness       0 .. max, read and written
 *   /sys/class/backlight/acpi_video0/max_brightness
 *   /sys/class/backlight/acpi_video0/actual_brightness
 *   /sys/class/backlight/acpi_video0/type             "firmware"
 *
 * show them. A write only records the number and wakes the acpi thread,
 * which runs _BCM: AML may take milliseconds (it can talk to the embedded
 * controller), and a system call never waits for it. The last number
 * written wins; ones in between are skipped.
 *
 * Firmware whose _BCM only asks the graphics driver to do it (many newer
 * Intel and AMD laptops) accepts the call and nothing changes: those need
 * the graphics card's own backlight register, later.
 */
#include <acpi.h>
#include <errno.h>
#include <interrupts.h>
#include <kprintf.h>
#include <string.h>
#include <vfs.h>

#include <uacpi/namespace.h>
#include <uacpi/uacpi.h>

#define LEVELS_MAX 101                  /* 0..100 percent, each once */

static uacpi_namespace_node *panel;
static uint8_t levels[LEVELS_MAX];      /* sorted, no repeats */
static int nlevels;
static volatile int wanted = -1, actual = -1;

/* The first device with both _BCL and _BCM: the panel. (A machine with
   two, an external output too, lists the built-in one first.) */
static uacpi_iteration_decision found(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user;
    (void)depth;
    uacpi_object_name n = uacpi_namespace_node_name(node);
    if (memcmp(n.text, "_BCM", 4))
        return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_namespace_node *dev = uacpi_namespace_node_parent(node);
    uacpi_namespace_node *bcl;
    if (uacpi_namespace_node_find(dev, "_BCL", &bcl) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    panel = dev;
    return UACPI_ITERATION_DECISION_BREAK;
}

/* _BCL's levels, sorted (insertion: at most 101, once at boot). */
static void read_levels(void)
{
    uacpi_object *o = NULL;
    uacpi_object_array a;
    if (uacpi_eval_typed(panel, "_BCL", NULL, UACPI_OBJECT_PACKAGE_BIT, &o) != UACPI_STATUS_OK ||
        uacpi_object_get_package(o, &a) != UACPI_STATUS_OK) {
        uacpi_object_unref(o);
        return;
    }
    for (uacpi_size i = 2; i < a.count; i++) {
        uint64_t v;
        if (uacpi_object_get_integer(a.objects[i], &v) != UACPI_STATUS_OK || v > 100)
            continue;
        int k = nlevels;
        while (k > 0 && levels[k - 1] > v)
            k--;
        if (k > 0 && levels[k - 1] == v)
            continue;
        memmove(&levels[k + 1], &levels[k], (uint32_t)(nlevels - k));
        levels[k] = (uint8_t)v;
        nlevels++;
    }
    uacpi_object_unref(o);
}

/* The number of the level the panel is at: _BQC's, or the highest one not
   above it. Without _BQC: the brightest (the firmware's start-up level is
   unknown, and that is what Linux assumes too). */
static int current_index(void)
{
    uint64_t v;
    if (uacpi_eval_simple_integer(panel, "_BQC", &v) != UACPI_STATUS_OK)
        return nlevels - 1;
    int k = 0;
    while (k + 1 < nlevels && levels[k + 1] <= v)
        k++;
    return k;
}

void backlight_init(void)
{
    uacpi_namespace_for_each_child(uacpi_namespace_root(), found, NULL,
                                   UACPI_OBJECT_METHOD_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    if (!panel)
        return;
    read_levels();
    if (nlevels < 2) {
        panel = NULL;
        return;
    }
    actual = current_index();
    klog("backlight: %d levels, %u%% to %u%%, now %u%%\n", nlevels, levels[0],
         levels[nlevels - 1], levels[actual]);
    sysfs_backlight_add();
}

/* The acpi thread: set what was asked for, if anything. */
void backlight_poll(void)
{
    uint32_t f = (uint32_t)irq_save();
    int w = wanted;
    wanted = -1;
    irq_restore(f);
    if (w < 0 || !panel)
        return;
    uacpi_object *arg = uacpi_object_create_integer(levels[w]);
    uacpi_object_array args = { &arg, 1 };
    if (uacpi_execute(panel, "_BCM", &args) == UACPI_STATUS_OK)
        actual = w;
    else
        klog("backlight: _BCM (%u) failed\n", levels[w]);
    uacpi_object_unref(arg);
}

int backlight_max(void)
{
    return panel ? nlevels - 1 : -1;
}

int backlight_get(void)
{
    int w = wanted;
    return w >= 0 ? w : actual;
}

int backlight_actual(void)
{
    return actual;
}

int backlight_set(int level)
{
    if (!panel)
        return -ENODEV;
    if (level < 0 || level >= nlevels)
        return -EINVAL;
    wanted = level;
    acpi_poke();
    return 0;
}
