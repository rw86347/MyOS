/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * machine.c — tell QEMU virt from Apple Silicon via the boot DTB (step 15a/15b)
 */
#include "apple/machine.h"

#include "fdt.h"

#include <stddef.h>
#include <string.h>

static int compat_has(const char *compat, const char *prefix)
{
    const char *p = compat;

    if (!compat || !prefix[0])
        return 0;
    while (*p) {
        size_t n = 0;

        while (p[n] && p[n] != ',')
            n++;
        if (!strncmp(p, prefix, strlen(prefix)))
            return 1;
        p += n;
        if (*p == ',')
            p++;
    }
    return 0;
}

enum arm64_platform arm64_platform_from_dtb(uintptr_t dtb_phys)
{
    char compat[128];

    if (fdt_check_header(dtb_phys) < 0)
        return ARM64_PLAT_UNKNOWN;
    if (fdt_root_prop(dtb_phys, "compatible", compat, sizeof compat) < 0)
        return ARM64_PLAT_UNKNOWN;
    if (compat_has(compat, "apple,"))
        return ARM64_PLAT_APPLE;
    if (compat_has(compat, "linux,dummy-virt") || compat_has(compat, "qemu"))
        return ARM64_PLAT_QEMU_VIRT;
    return ARM64_PLAT_UNKNOWN;
}

void arm64_platform_label(uintptr_t dtb_phys, char *out, size_t outsz)
{
    char compat[128];
    char model[128];
    enum arm64_platform p = arm64_platform_from_dtb(dtb_phys);

    if (!out || !outsz)
        return;
    out[0] = '\0';
    if (p == ARM64_PLAT_QEMU_VIRT) {
        strncpy(out, "QEMU virt", outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    if (p == ARM64_PLAT_APPLE) {
        if (fdt_root_prop(dtb_phys, "model", model, sizeof model) == 0 &&
            model[0]) {
            strncpy(out, model, outsz - 1);
            out[outsz - 1] = '\0';
        } else if (fdt_root_prop(dtb_phys, "compatible", compat, sizeof compat) ==
                   0) {
            strncpy(out, compat, outsz - 1);
            out[outsz - 1] = '\0';
        } else {
            strncpy(out, "Apple Silicon", outsz - 1);
            out[outsz - 1] = '\0';
        }
        return;
    }
    strncpy(out, "unknown platform", outsz - 1);
    out[outsz - 1] = '\0';
}
