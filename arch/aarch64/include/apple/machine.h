/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_AARCH64_APPLE_MACHINE_H
#define ARCH_AARCH64_APPLE_MACHINE_H

#include <stddef.h>
#include <stdint.h>

enum arm64_platform {
    ARM64_PLAT_UNKNOWN = 0,
    ARM64_PLAT_QEMU_VIRT,
    ARM64_PLAT_APPLE,
};

enum arm64_platform arm64_platform_from_dtb(uintptr_t dtb_phys);
void arm64_platform_label(uintptr_t dtb_phys, char *out, size_t outsz);

#endif
