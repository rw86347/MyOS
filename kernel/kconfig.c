/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * kconfig.c — the kernel config: which config the kernel runs with
 *
 * Two places it can come from (docs/KERNEL-CONFIG.md §8):
 *   1. the built-in default, linked in by the build from config/slots.conf
 *      (tools/kconfig/kconfig.py cdefault): what the kernel starts with;
 *   2. a myos-config partition, which Settings › Kernel writes: read in
 *      storage_init once the root disk is known, and used if a copy is good.
 *
 * Both are checked the same way, as `kconfig.py decode` checks (the two
 * must agree: tools/kconfig/kconfig.py is the reference). A part is used
 * only if every field makes sense: the right magic, version, byte order
 * and arch, a CRC that matches, the OS in slots 0 and 10, owners that
 * name real tasks, names a program could have. The partition holds two
 * copies, A and B, each a boot block and a task list with the same
 * generation; the newer good copy wins, and a save cut off between its
 * two writes leaves the other copy in use. Nothing here is on a hot path:
 * it runs once at boot, so it is written to be plain, not fast.
 *
 * What the config does today: it gives the scheduler its slot owners
 * (sched.c asks kconfig_slot_owner). The modes, binding by program and
 * the power savings are later steps (§10).
 */
#include <blockdev.h>
#include <errno.h>
#include <kconfig.h>
#include <kheap.h>
#include <kprintf.h>
#include <linux/crc32.h>
#include <partition.h>
#include <string.h>

#ifdef ARCH_X86_64
#define ARCH_SELF KCFG_ARCH_X86_64
#elif defined(ARCH_AARCH64)
#define ARCH_SELF KCFG_ARCH_ARM64
#else
#define ARCH_SELF KCFG_ARCH_I386
#endif
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ORDER_SELF 1                    /* we never swap bytes: a config in */
#else                                   /* the other order is for another machine */
#define ORDER_SELF 2
#endif

#define SLOT_US      1000
#define TASKS_BYTES  5120
#define ENTRY_BYTES  256
#define PM_KNOWN     ((1u << 17) - 1)   /* the 17 power features of §5 */

extern const uint8_t kconfig_default_boot[512];
extern const uint8_t kconfig_default_tasks[5120];

static struct kconfig boot;             /* the config in use */
static struct kconfig_tasks tasks;
static char source[24] = "built-in";    /* where it came from, for the log */

/* --- the checks ------------------------------------------------------------- */

uint32_t kconfig_crc(const void *p, uint32_t n, uint32_t at)
{
    static const uint8_t zero[4];
    const uint8_t *b = p;
    uint32_t c = crc32_le(~0u, b, at);
    c = crc32_le(c, zero, 4);
    return ~crc32_le(c, b + at + 4, n - at - 4);
}

int kconfig_newer(uint32_t a, uint32_t b)
{
    return a - b != 0 && a - b < 0x80000000u;
}

/* A task name: 1-15 characters, a small letter first, then a-z 0-9 - _,
   NUL-padded to 16; and none the kernel uses itself. */
static int good_name(const char n[16])
{
    static const char *const reserved[] = { "os", "idle", "main", "kernel" };
    int len = 0;
    while (len < 16 && n[len])
        len++;
    if (len == 0 || len == 16 || n[0] < 'a' || n[0] > 'z')
        return 0;
    for (int i = 1; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    for (int i = len; i < 16; i++)
        if (n[i])
            return 0;                   /* padded with NULs, as Save writes it */
    for (unsigned r = 0; r < sizeof reserved / sizeof reserved[0]; r++)
        if (!strcmp(n, reserved[r]))
            return 0;
    return 1;
}

const char *kconfig_check_boot(const struct kconfig *b)
{
    if (memcmp(b->magic, KCFG_MAGIC, 8))
        return "no MYOSKCFG magic (never written?)";
    if (b->byte_order != ORDER_SELF)
        return "written in the other byte order (for another machine)";
    if (b->version != KCFG_VERSION || b->size != 512)
        return "a version this kernel doesn't know";
    if (b->crc32 != kconfig_crc(b, 512, 0x14))
        return "the CRC doesn't match (damaged or half written)";
    if (b->arch != ARCH_SELF)
        return "written for another processor family";
    if (b->nslots != KCFG_SLOTS || b->slot_us != SLOT_US ||
        b->os_slot[0] != 0 || b->os_slot[1] != 10)
        return "a slot layout this kernel doesn't have";
    if (b->mode > KCFG_POWER_SAVING)
        return "an unknown mode";
    if (b->ntasks > KCFG_MAX_TASKS)
        return "more than 18 tasks";
    for (int i = 0; i < b->ntasks; i++) {
        if (!good_name(b->task_name[i]))
            return "a task name a program can't have";
        for (int j = 0; j < i; j++)
            if (!memcmp(b->task_name[i], b->task_name[j], 16))
                return "the same task named twice";
    }
    uint32_t used = 0;
    int nfree = 0;
    for (int s = 0; s < KCFG_SLOTS; s++) {
        uint8_t o = b->slot_owner[s];
        if ((s == 0 || s == 10) != (o == KCFG_OWNER_OS))
            return "slots 0 and 10, and only they, are the OS's";
        if (o == KCFG_OWNER_FREE)
            nfree++;
        else if (o != KCFG_OWNER_OS && o > b->ntasks)
            return "a slot names a task that isn't there";
        else if (o != KCFG_OWNER_OS)
            used |= 1u << (o - 1);
    }
    if (used != (b->ntasks ? (1u << b->ntasks) - 1 : 0))
        return "a task with no slot";
    if (!(b->rt_flags & KCFG_RT_LEFTOVER) && nfree == 0)
        return "no free slot and no leftover time: nothing else could run";
    if (b->mode == KCFG_SOFT_REALTIME &&
        (b->latency_budget_us < 1 || b->latency_budget_us > 1000))
        return "a latency budget outside 1-1000 us";
    if ((b->power_on | b->power_seen) & ~PM_KNOWN)
        return "a power feature this kernel doesn't know";
    if (b->mode == KCFG_POWER_SAVING) {
        if (b->cpu_min_pct < 10 || b->cpu_min_pct > b->cpu_max_pct || b->cpu_max_pct > 100)
            return "processor speeds outside 10% <= slowest <= fastest <= 100%";
        if (b->cstate_max < 1 || b->cstate_max > 10)
            return "an idle state outside C1-C10";
        if ((b->power_on & KCFG_PM_DISK_SPINDOWN) &&
            (b->spindown_min < 1 || b->spindown_min > 600))
            return "a disk spin-down outside 1-600 minutes";
    }
    return NULL;
}

/* An absolute path a program can have: printable, no "//", no "/../". */
static int good_program(const char p[128])
{
    int len = 0;
    while (len < 128 && p[len])
        len++;
    if (len == 0 || len == 128 || p[0] != '/')
        return 0;
    for (int i = 0; i < len; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E)
            return 0;
        if (p[i] == '/' && (p[i + 1] == '/' ||
            (p[i + 1] == '.' && p[i + 2] == '.' && (p[i + 3] == '/' || p[i + 3] == 0))))
            return 0;
    }
    return 1;
}

/* Arguments: printable strings, each NUL-terminated, then an empty one,
   all inside the 96 bytes. */
static int good_args(const char a[96])
{
    int i = 0;
    while (i < 96 && a[i]) {
        while (i < 96 && a[i]) {
            if (a[i] < 0x20 || a[i] > 0x7E)
                return 0;
            i++;
        }
        i++;                            /* past this argument's NUL */
    }
    return i < 96;                      /* the empty one fits */
}

const char *kconfig_check_tasks(const struct kconfig_tasks *t, const struct kconfig *b)
{
    if (memcmp(t->magic, KCFG_TASK_MAGIC, 8))
        return "no MYOSTASK task list";
    if (t->byte_order != b->byte_order || t->version != KCFG_VERSION ||
        t->size != TASKS_BYTES || t->entry_size != ENTRY_BYTES)
        return "a task list this kernel doesn't know";
    if (t->crc32 != kconfig_crc(t, TASKS_BYTES, 0x14))
        return "the task list's CRC doesn't match (damaged or half written)";
    if (t->generation != b->generation)
        return "the task list is from another save (one was cut off)";
    if (t->ntasks != b->ntasks)
        return "the task list doesn't list the boot block's tasks";
    for (int i = 0; i < t->ntasks; i++) {
        const struct kconfig_task *e = &t->task[i];
        if (memcmp(e->name, b->task_name[i], 16))
            return "a task list entry isn't the boot block's task";
        if (!good_program(e->program))
            return "a task's program isn't an absolute path";
        if (!good_args(e->args))
            return "a task's arguments don't fit";
        if (e->restart_ms > 60000)
            return "a task's restart wait is over 60 s";
    }
    return NULL;
}

/* --- choosing ---------------------------------------------------------------- */

int kconfig_pick(const uint8_t *image, const struct kconfig **bp,
                 const struct kconfig_tasks **tp, const char *why[2])
{
    static const struct { uint8_t boot, tasks; } at[2] = {
        { KCFG_BOOT_SECTOR_A, KCFG_TASKS_SECTOR_A },
        { KCFG_BOOT_SECTOR_B, KCFG_TASKS_SECTOR_B },
    };
    int best = -1;
    for (int c = 0; c < 2; c++) {
        const struct kconfig *b = (const void *)(image + at[c].boot * 512);
        const struct kconfig_tasks *t = (const void *)(image + at[c].tasks * 512);
        why[c] = kconfig_check_boot(b);
        if (!why[c])
            why[c] = kconfig_check_tasks(t, b);
        if (why[c])
            continue;
        if (best < 0 || kconfig_newer(b->generation, (*bp)->generation)) {
            best = c;
            *bp = b;
            *tp = t;
        }
    }
    return best < 0 ? 0 : 'A' + best;
}

/* --- in use ------------------------------------------------------------------- */

const struct kconfig *kconfig_boot(void) { return &boot; }
const struct kconfig_tasks *kconfig_tasks(void) { return &tasks; }

const char *kconfig_slot_owner(int s)
{
    if (s < 0 || s >= KCFG_SLOTS)
        return NULL;
    uint8_t o = boot.slot_owner[s];
    if (o == KCFG_OWNER_OS)
        return "main";
    return o == KCFG_OWNER_FREE ? NULL : boot.task_name[o - 1];
}

/* One line for the log, under the current step: where the config came
   from, the mode, and each task's slots. Not the screen — a normal boot
   shows the steps and nothing else. */
static void say(void)
{
    static const char *const modes[] = { "realtime", "soft realtime", "power saving" };
    klog("kconfig: %s, generation %lu: %s", source, (unsigned long)boot.generation,
         modes[boot.mode]);
    for (int i = 0; i < boot.ntasks; i++) {
        klog(i ? "; %s" : ": %s", boot.task_name[i]);
        for (int s = 0; s < KCFG_SLOTS; s++)
            if (boot.slot_owner[s] == i + 1)
                klog(" %d", s);
    }
    klog("\n");
}

static void use(const struct kconfig *b, const struct kconfig_tasks *t)
{
    memcpy(&boot, b, sizeof boot);
    memcpy(&tasks, t, sizeof tasks);
}

void kconfig_init(void)
{
    const struct kconfig *b = (const void *)kconfig_default_boot;
    const struct kconfig_tasks *t = (const void *)kconfig_default_tasks;
    const char *why = kconfig_check_boot(b);
    if (!why)
        why = kconfig_check_tasks(t, b);
    if (why) {
        /* The build checked it, so this "can't happen": run with the OS
           slots only rather than with a table we don't trust. */
        kprintf("kconfig: the built-in default is bad (%s): no task slots\n", why);
        memset(&boot, 0, sizeof boot);
        memset(boot.slot_owner, KCFG_OWNER_FREE, sizeof boot.slot_owner);
        boot.slot_owner[0] = boot.slot_owner[10] = KCFG_OWNER_OS;
        boot.rt_flags = KCFG_RT_LEFTOVER;
        return;
    }
    use(b, t);
    say();
}

/* A myos-config partition: named so in a GPT, or (behind a hybrid MBR,
   where the kernel reads only the MBR) an unnamed entry of type 0xDA. A
   foreign 0xDA partition is harmless: reading it finds no good copy. */
static int config_partition(const struct blockdev *d)
{
    return d->parent && (!strcmp(d->label, "myos-config") ||
                         (!d->label[0] && partition_type(d) == 0xDA));
}

/* Read d's copies; use the newer good one. 1 if it did. */
static int try_partition(struct blockdev *d)
{
    if (d->sectors < KCFG_PARTITION_SECTORS)
        return 0;
    uint8_t *image = kmalloc(KCFG_PARTITION_SECTORS * 512);
    if (!image)
        return 0;
    const struct kconfig *b;
    const struct kconfig_tasks *t;
    const char *why[2] = { "can't read it", "can't read it" };
    int which = blockdev_read(d, 0, KCFG_PARTITION_SECTORS, image) < 0 ? 0
              : kconfig_pick(image, &b, &t, why);
    if (which) {
        use(b, t);
        uint32_t n = strlen(d->name);
        if (n > sizeof source - 8)
            n = sizeof source - 8;
        memcpy(source, d->name, n);
        memcpy(source + n, which == 'A' ? " copy A" : " copy B", 8);
        say();
        if (why[which == 'A'])          /* the other one: worth knowing */
            klog("kconfig: (%s copy %c: %s)\n", d->name, which == 'A' ? 'B' : 'A',
                 why[which == 'A']);
    } else {
        /* An empty or foreign 0xDA partition: the built-in config stands.
           The log keeps why; the screen stays on the steps. */
        klog("kconfig: %s: no good copy (A: %s; B: %s): keeping the %s config\n",
             d->name, why[0], why[1], source);
    }
    kfree(image);
    return which != 0;
}

void kconfig_from_disk(const struct blockdev *disk)
{
    int seen = 0;
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_get(i);
        if (!config_partition(d) || (disk && d->parent != disk))
            continue;
        seen = 1;
        if (try_partition(d))
            return;                     /* the first good one: the root disk's */
    }
    if (!seen && disk)
        klog("kconfig: no myos-config partition on %s: the %s config\n", disk->name, source);
}
