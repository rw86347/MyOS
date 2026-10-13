/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * kconfig_test.c — the kernel config's checks, fed good and damaged copies
 * (docs/KERNEL-CONFIG.md §10 step 2).
 *
 * A myos-config partition is built in memory from the built-in default:
 * copy A and copy B, each a boot block and a task list. Then each kind of
 * damage the kernel must refuse is made in turn — a bad CRC, the other
 * byte order, another processor's config, a save cut off between its two
 * writes — and kconfig_pick must fall back to the other copy, or to none.
 */
#include <kconfig.h>
#include <kheap.h>
#include <kprintf.h>
#include <selftest.h>
#include <string.h>

extern const uint8_t kconfig_default_boot[512];
extern const uint8_t kconfig_default_tasks[5120];

static uint8_t *image;                  /* KCFG_PARTITION_SECTORS sectors */

static struct kconfig *boot_of(int copy)
{
    return (struct kconfig *)(image + (copy ? KCFG_BOOT_SECTOR_B : KCFG_BOOT_SECTOR_A) * 512);
}

static struct kconfig_tasks *tasks_of(int copy)
{
    return (struct kconfig_tasks *)(image + (copy ? KCFG_TASKS_SECTOR_B : KCFG_TASKS_SECTOR_A) * 512);
}

/* What Save writes: both parts with one generation, CRCs to match. */
static void write_copy(int copy, uint32_t generation)
{
    struct kconfig *b = boot_of(copy);
    struct kconfig_tasks *t = tasks_of(copy);
    memcpy(b, kconfig_default_boot, 512);
    memcpy(t, kconfig_default_tasks, 5120);
    b->generation = t->generation = generation;
    b->crc32 = kconfig_crc(b, 512, 0x14);
    t->crc32 = kconfig_crc(t, 5120, 0x14);
}

static void reseal(struct kconfig *b) { b->crc32 = kconfig_crc(b, 512, 0x14); }
static void reseal_tasks(struct kconfig_tasks *t) { t->crc32 = kconfig_crc(t, 5120, 0x14); }

static int pick(void)
{
    const struct kconfig *b;
    const struct kconfig_tasks *t;
    const char *why[2];
    return kconfig_pick(image, &b, &t, why);
}

void kconfig_selftest(void)
{
    image = kmalloc(KCFG_PARTITION_SECTORS * 512);
    if (!image) {
        check(0, "kconfig: memory for a test partition");
        return;
    }
    memset(image, 0, KCFG_PARTITION_SECTORS * 512);
    const struct kconfig *in_use = kconfig_boot();
    check(in_use->ntasks == 2 && kconfig_slot_owner(0) && !strcmp(kconfig_slot_owner(0), "main") &&
          kconfig_slot_owner(4) && !strcmp(kconfig_slot_owner(4), "brakes") &&
          !kconfig_slot_owner(1), "kconfig: the built-in default is in use (slots 0, 4, 1)");

    check(pick() == 0, "kconfig: an empty partition has no good copy");
    write_copy(0, 5);
    check(pick() == 'A', "kconfig: copy A alone");
    write_copy(1, 6);
    check(pick() == 'B', "kconfig: the newer of two good copies");
    write_copy(0, 0xFFFFFFFFu);
    write_copy(1, 1);
    check(pick() == 'B', "kconfig: generations wrap: 1 is newer than 0xFFFFFFFF");

    /* Damage copy B (the newer); A must be used. */
    write_copy(0, 5);
    write_copy(1, 6);
    boot_of(1)->task_name[0][0] ^= 1;   /* a flipped bit, CRC left alone */
    check(pick() == 'A' && kconfig_check_boot(boot_of(1)), "kconfig: a bad CRC is refused");

    write_copy(1, 6);
    boot_of(1)->byte_order = 2;
    reseal(boot_of(1));
    check(pick() == 'A', "kconfig: the other byte order is refused");

    write_copy(1, 6);
    boot_of(1)->arch = boot_of(1)->arch == KCFG_ARCH_I386 ? KCFG_ARCH_X86_64 : KCFG_ARCH_I386;
    reseal(boot_of(1));
    check(pick() == 'A', "kconfig: another processor's config is refused");

    write_copy(1, 6);                   /* a save cut off: new task list, old block */
    tasks_of(1)->generation = 7;
    reseal_tasks(tasks_of(1));
    check(pick() == 'A', "kconfig: a task list from another save is refused");

    write_copy(1, 6);
    boot_of(1)->slot_owner[10] = 1;     /* a task in the OS's slot */
    reseal(boot_of(1));
    check(pick() == 'A', "kconfig: the OS keeps slots 0 and 10");

    write_copy(1, 6);
    boot_of(1)->slot_owner[3] = 9;      /* a task that isn't there */
    reseal(boot_of(1));
    check(pick() == 'A', "kconfig: a slot naming a missing task is refused");

    write_copy(1, 6);
    memcpy(tasks_of(1)->task[0].program, "/bin/../etc/x", 14);
    reseal_tasks(tasks_of(1));
    check(pick() == 'A', "kconfig: a program path with .. is refused");

    write_copy(0, 5);                   /* both bad: none, the kernel keeps its own */
    boot_of(0)->magic[0] = 'X';
    check(pick() == 0, "kconfig: two bad copies: none");
    kfree(image);
}
