/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * acpi_button.c — the lid and the power button, from ACPI; and what the
 * firmware says about sleeping (the boot log)
 *
 * A laptop's lid is a device with the id PNP0C0D and a method _LID (1:
 * open). When it moves, the firmware's AML (an embedded-controller query,
 * _Qxx, most often) does Notify(LID0, 0x80); we run _LID again and tell
 * evdev.c (SW_LID on event3) and, when it opened, kernel/suspend.c.
 *
 * The power button is either "fixed" (a bit in the PM1 registers, raised
 * through ACPI's interrupt; acpi_os.c's handler calls acpi_button_power)
 * or a device, PNP0C0C, that the AML notifies: 0x80 pressed, 0x02 it woke
 * the machine. Either way it is KEY_POWER on event4, or KEY_WAKEUP if it
 * woke the machine (so the power daemon doesn't put it back to sleep).
 *
 * _PRW says which GPE (general-purpose event) a device wakes the machine
 * by. In s2idle the machine is still in S0, so the GPEs that run AML work
 * as at any other time. A lid's GPE with no AML of its own is turned on
 * here, as an "implicit notify": it comes to us as Notify(LID0, 2).
 *
 * The boot log says what a port to new hardware (the P2 Max first) needs
 * to know: does the firmware offer s2idle (the FADT's "low-power S0 idle")
 * or S3 (\_S3_), where the lid is, what wakes it, is it open.
 *
 * Everything here but acpi_button_power runs in the acpi thread.
 */
#include <acpi.h>
#include <evdev.h>
#include <kprintf.h>
#include <suspend.h>

#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

static uacpi_namespace_node *lid;
static volatile int lid_shut;

int acpi_lid_closed(void)
{
    return lid_shut;
}

/* Any context: the fixed button's handler runs in ACPI's interrupt. */
void acpi_button_power(void)
{
    evdev_power(suspend_wake(WAKE_POWER) ? EVDEV_KEY_WAKEUP : EVDEV_KEY_POWER);
}

static void lid_read(void)
{
    uacpi_u64 open;
    if (uacpi_eval_simple_integer(lid, "_LID", &open) != UACPI_STATUS_OK)
        return;
    lid_shut = open == 0;
    evdev_switch(EVDEV_SW_LID, lid_shut);   /* (an event only if it changed) */
    if (!lid_shut)
        suspend_wake(WAKE_LID);
}

static uacpi_status lid_notified(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx, (void)node;
    lid_read();
    klog("lid: %s (notify 0x%x)\n", lid_shut ? "shut" : "open", (unsigned)value);
    return UACPI_STATUS_OK;
}

static uacpi_status power_notified(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx, (void)node;
    if (value == 0x80)
        acpi_button_power();
    else if (value == 0x02)
        suspend_wake(WAKE_POWER);       /* "it woke the machine": not a press */
    return UACPI_STATUS_OK;
}

/* Where device dev is, for the log ("\_SB.PCI0.LPCB.LID0"). */
static void log_path(const char *what, uacpi_namespace_node *dev, const char *more)
{
    const char *path = uacpi_namespace_node_generate_absolute_path(dev);
    klog("acpi: %s: %s%s\n", what, path ? path : "?", more);
    uacpi_free_absolute_path(path);
}

/* _PRW: { the GPE, or { its block device, the GPE }; the deepest sleep
   state it wakes from; ... }. The lid's GPE, if no AML handles it, is
   turned on to notify the lid. */
static void wake_gpe(const char *what, uacpi_namespace_node *dev)
{
    uacpi_object *o = NULL;
    uacpi_object_array a;
    uacpi_u64 gpe, state = 0;
    if (uacpi_eval_typed(dev, "_PRW", NULL, UACPI_OBJECT_PACKAGE_BIT, &o) != UACPI_STATUS_OK ||
        uacpi_object_get_package(o, &a) != UACPI_STATUS_OK || a.count < 2) {
        klog("acpi: %s: no _PRW (it does not say it wakes the machine)\n", what);
        uacpi_object_unref(o);
        return;
    }
    uacpi_object_get_integer(a.objects[1], &state);
    if (uacpi_object_get_integer(a.objects[0], &gpe) != UACPI_STATUS_OK) {
        klog("acpi: %s: wakes from S%u by a GPE block device's event (not set up)\n",
             what, (unsigned)state);
        uacpi_object_unref(o);
        return;
    }
    uacpi_object_unref(o);
    uacpi_event_info info = 0;
    uacpi_gpe_info(NULL, (uacpi_u16)gpe, &info);
    const char *how = info & UACPI_EVENT_INFO_ENABLED ? "its AML runs" : "off";
    if (dev == lid && !(info & UACPI_EVENT_INFO_ENABLED) &&
        uacpi_setup_gpe_for_wake(NULL, (uacpi_u16)gpe, dev) == UACPI_STATUS_OK &&
        uacpi_enable_gpe(NULL, (uacpi_u16)gpe) == UACPI_STATUS_OK)
        how = "on, notifies the lid";
    klog("acpi: %s: wakes from S%u by GPE 0x%02x (%s)\n", what, (unsigned)state,
         (unsigned)gpe, how);
}

static uacpi_iteration_decision found_lid(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user, (void)depth;
    if (lid)
        return UACPI_ITERATION_DECISION_BREAK;     /* one lid */
    lid = node;
    lid_read();
    log_path("lid", node, lid_shut ? ", shut" : ", open");
    wake_gpe("lid", node);
    uacpi_install_notify_handler(node, lid_notified, NULL);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_power(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user, (void)depth;
    log_path("power button", node, " (PNP0C0C)");
    wake_gpe("power button", node);
    uacpi_install_notify_handler(node, power_notified, NULL);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_button_init(void)
{
    struct acpi_fadt *fadt = NULL;
    uacpi_namespace_node *s3 = NULL;
    uacpi_table_fadt(&fadt);
    uacpi_namespace_node_find(NULL, "\\_S3_", &s3);
    klog("acpi: sleep: s2idle (low-power S0 idle) %s, S3 %s\n",
         fadt && (fadt->flags & ACPI_LOW_POWER_S0_IDLE_CAPABLE) ? "yes" : "no", s3 ? "yes" : "no");
    if (fadt && !(fadt->flags & ACPI_PWR_BUTTON))
        klog("acpi: power button: fixed (the FADT's)\n");
    uacpi_find_devices("PNP0C0C", found_power, NULL);
    uacpi_find_devices("PNP0C0D", found_lid, NULL);
    if (!lid)
        klog("acpi: lid: none\n");
#ifdef SLEEP_TEST
    fakelid_init();                     /* make test-sleep: QEMU's lid */
#endif
}
