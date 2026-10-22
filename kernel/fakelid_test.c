/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * fakelid_test.c — QEMU's lid, for make test-sleep (SLEEPTEST=1 builds)
 *
 * QEMU has no lid. tools/fakelid.py's ACPI table gives it one (\_SB.LID0,
 * open, and a method TOGL that shuts or opens it and notifies, as a real
 * lid's EC query would), but nothing on the host can run that method by
 * itself. Something QEMU does have AML for can be poked from its monitor:
 * CPU hotplug (-smp 1,maxcpus=N). device_add of a CPU raises GPE 2, whose
 * AML notifies the new CPU's object ("device check"); that
 * notify runs TOGL here. MyOS uses one CPU, so the new one stays in reset.
 * The lid's own path (acpi_button.c) then sees a lid that moved.
 *
 * Test code: a build folder of its own, never a normal boot.
 */
#include <acpi.h>
#include <kprintf.h>

#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

static int cpus;

static uacpi_status cpu_notified(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx, (void)node;
    if (value == 1)                     /* device check: a CPU came */
        uacpi_eval(NULL, "\\_SB.LID0.TOGL", NULL, NULL);
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)user, (void)depth;
    if (uacpi_install_notify_handler(node, cpu_notified, NULL) == UACPI_STATUS_OK)
        cpus++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void fakelid_init(void)
{
    /* QEMU's CPUs are ACPI 1.0 Processor objects; newer firmware's are
       devices with _HID ACPI0007. */
    uacpi_namespace_for_each_child(uacpi_namespace_root(), found, NULL,
                                   UACPI_OBJECT_PROCESSOR_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    uacpi_find_devices("ACPI0007", found, NULL);
    klog("fakelid: a CPU added in QEMU's monitor opens or shuts the lid (%d CPU slots)\n", cpus);
}
