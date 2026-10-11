/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/* Static musl smoke test for MyOS aarch64 (roadmap 14b). */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    const char msg[] = "musl hello from MyOS\n";

    write(1, msg, sizeof msg - 1);
    return 0;
}
