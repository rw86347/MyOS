/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * main.c — the kernel's C entry point
 *
 * By the time we get here:
 *   BIOS -> MBR -> stage 2 (real mode: memory map, kernel + initramfs loaded
 *   above 1 MiB, protected mode) -> entry.asm (cleared .bss, turned on
 *   paging, jumped to the higher half at 0xC0000000) -> kernel_main()
 *
 * Three kinds of build (see the Makefile):
 *   make         a normal boot: no test or benchmark code at all
 *   make test    SELFTEST: runs the self-tests, then the normal loop
 *   make bench   BENCH: runs the benchmarks, then switches QEMU off
 */
#include <ata.h>
#include <drm/drm_drv.h>
#include <virtio_blk.h>
#include <ahci.h>
#include <nvme.h>
#include <bcache.h>
#include <pcache.h>
#include <blockdev.h>
#include <bootinfo.h>
#include <cpu.h>
#include <ext2.h>
#include <fat32.h>
#include <string.h>
#include <cycles.h>
#include <fpu.h>
#include <gdt.h>
#include <initrd.h>
#include <interrupts.h>
#include <kheap.h>
#include <keyboard.h>
#include <acpi.h>
#include <kconfig.h>
#include <kprintf.h>
#include <memlayout.h>
#include <mouse.h>
#include <netdevice.h>
#include <mm.h>
#include <proc.h>
#include <vfs.h>
#include <paging.h>
#include <pci.h>
#include <partition.h>
#include <pmm.h>
#include <random.h>
#include <sched.h>
#include <serial.h>
#include <shim.h>
#include <sound/hdaudio.h>
#include <sysenter.h>
#include <thread.h>
#include <syscalls.h>
#include <timer.h>
#include <tty.h>
#include <usb.h>
#include <vga.h>
#include <vt.h>
#ifdef SELFTEST
#include <selftest.h>
#endif
#ifdef BENCH
#include <bench.h>
#endif

extern char _start[];       /* first byte of the kernel (entry.asm) */

#if !defined(SELFTEST) && !defined(BENCH)
/* Start process 1 and wait for it (main sleeps, so its system slots go to
   the programs). Like Linux: /init first (the initramfs's: BusyBox), then
   where a distribution's root file system keeps its init (the live disk:
   /sbin/init is runit). Returns only if none of them exists. */
static void run_init(void)
{
    static const char *const paths[] = {
        "/init", "/sbin/init", "/etc/init", "/bin/init", "/bin/sh",
    };
    static const char *const envp[] = {
        "HOME=/", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "TERM=linux", NULL,
    };
    struct inode *ino;
    const char *path = NULL;
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0] && !path; i++)
        if (vfs_lookup(paths[i], &ino) == 0)
            path = paths[i];
    if (!path)
        return;                         /* nothing to run: stay in the kernel */
    const char *const argv[] = { path, NULL };
    struct process *p = proc_spawn(path, argv, envp);
    if (!p) {
        kprintf("Could not start %s\n", path);
        return;
    }
    proc_set_init(p);
    tty_set_foreground(p->pgid);        /* Ctrl-C reaches init's group */
    tty_attach_keyboard();              /* keys go to the terminal now */
    int status = proc_wait_kernel(p);
    kprintf("\n%s exited (status %d).\n", path, status);
}
#endif

/* Show "up 0:01:23" in the top-right corner of the screen. */
static void show_uptime(uint32_t secs)
{
    char s[] = "up 0:00:00";
    uint32_t h = secs / 3600, m = secs / 60 % 60, sec = secs % 60;
    s[3] = '0' + h % 10;
    s[5] = '0' + m / 10;   s[6] = '0' + m % 10;
    s[8] = '0' + sec / 10; s[9] = '0' + sec % 10;
    vt_write_at(vt_cols() - (int)sizeof s + 1, 0, s, VT_BLACK, VT_LIGHT_GREY);
}

/* Echo typed keys. Control characters are shown as ^C, ^D, ... */
static void echo(int c)
{
    if (c == '\n')
        kprintf("\n> ");
    else if (c == '\b')
        kprintf("\b \b");                 /* back, blank, back */
    else if (c == '\t')
        kprintf(" ");
    else if (c < 32)
        kprintf("^%c", c + '@');
    else
        kprintf("%c", c);
}

/* Does any partition live on this disk? */
static int has_partitions(struct blockdev *disk)
{
    for (int i = 0; i < blockdev_count(); i++)
        if (blockdev_get(i)->parent == disk)
            return 1;
    return 0;
}

/* The whole system on a disk: mount it as "/" and put the device files
   (/dev, made in RAM at boot) on its /dev. Before any process exists. */
static void mount_root(struct blockdev *d)
{
    struct inode *ram_dev = vfs_child(vfs_root(), "dev", 3);
    if (ext2_mount(d, "/") < 0)
        return;                         /* stay on the RAM file system */
    if (ram_dev && vfs_mount_tree("/dev", ram_dev) < 0)
        kprintf("root: no /dev on %s\n", d->name);
}

static const struct blockdev *disk_of_part(const struct blockdev *d)
{
    return d->parent ? d->parent : d;
}

/* Two disks may both hold a "myos-root": the USB stick MyOS was installed
   from, still plugged in, and the disk it was installed to. The one on the
   kind of disk we started from wins (1), so the stick can't take over the
   installed system's start, nor the installed system the stick's. Disk
   order alone would pick whichever driver found its disk first. */
static int root_rank(const struct blockdev *d)
{
    const struct blockdev *disk = disk_of_part(d);
    int usb_start = 0;
#ifdef BOOT_FROM_USB
    usb_start = (((const struct bootinfo *)P2V(BOOTINFO_PHYS))->boot_flags & BOOT_FROM_USB) != 0;
#endif
    return (disk->removable != 0) == usb_start;
}

/* Disks: find them and their partitions, then our own file systems on
   them, by label — on a Linux partition (type 0x83) or a whole disk with
   no partition table:
     "myos-root"  the whole system: becomes "/" (an installed MyOS)
     "myos-data"  becomes /data
   Without a "myos-root", MyOS runs from the initramfs in RAM (a live
   disk). Anything else is left alone: an old PC may still hold someone's
   Linux disk, and we never write to that. */
/* The disks from index `from` on: their partitions, then our labels.
   Returns how far it got (the usb thread may add disks meanwhile).
   A "myos-data" belongs with its root: only one on the root's disk is
   taken (storage_init), so a live stick never mounts an installed
   system's files. */
#define DATA_MAX 4
static struct blockdev *data_seen[DATA_MAX];
static int ndata_seen;

static int scan_disks(int from, struct blockdev **rootdev)
{
    for (int i = from; i < blockdev_count(); i++)      /* adds the partitions */
        if (!blockdev_get(i)->parent)
            partition_scan(blockdev_get(i));
    int i;
    for (i = from; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_get(i);
        char label[17];
        if (d->parent ? partition_type(d) != 0x83 : has_partitions(d))
            continue;
        if (ext2_label(d, label) < 0)
            continue;                   /* not ext2 */
        if (strcmp(label, "myos-root") == 0) {
            struct blockdev *other = d;
            if (!*rootdev || root_rank(d) > root_rank(*rootdev)) {
                other = *rootdev;
                *rootdev = d;
            }
            if (other)
                klog("ext2: %s is another \"myos-root\": left alone\n", other->name);
        } else if (strcmp(label, "myos-data") == 0) {
            if (ndata_seen < DATA_MAX)
                data_seen[ndata_seen++] = d;
        } else
            klog("ext2: %s (\"%s\") is not a MyOS disk: left alone\n", d->name, label);
    }
    return i;
}

static void storage_init(void)
{
    kconfig_init();                     /* the built-in config, until a disk's */
    boot_step("Disks and USB");
    usb_init();                         /* USB sticks: found by the usb thread, */
    ata_init();                         /* while these start */
    virtio_blk_init();
    ahci_init();
    nvme_init();
    usb_wait_boot(8000);
    bcache_init();                      /* blocks, cached */
    pcache_init();                      /* and files' pages */

    boot_step("File systems");
    struct blockdev *rootdev = NULL, *datadev = NULL;
    int seen = scan_disks(0, &rootdev);
#ifdef BOOT_FROM_USB
    /* We came from a USB stick, so its myos-root should be here: a slow
       stick may show up late. Wait for it a while (Linux's "rootwait"). */
    const struct bootinfo *bi = P2V(BOOTINFO_PHYS);
    if ((!rootdev || !root_rank(rootdev)) && (bi->boot_flags & BOOT_FROM_USB)) {
        kprintf("storage: waiting for the USB stick we started from\n");
        for (int t = 0; t < 100 && (!rootdev || !root_rank(rootdev)); t++) {
            usb_sleep(100);
            seen = scan_disks(seen, &rootdev);
        }
    }
#endif
    (void)seen;
    if (!blockdev_count())
        kprintf("storage: no disks\n");
    for (int k = 0; k < ndata_seen && !datadev; k++)
        if (!rootdev || disk_of_part(data_seen[k]) == disk_of_part(rootdev))
            datadev = data_seen[k];
    for (int k = 0; k < ndata_seen; k++)
        if (data_seen[k] != datadev)
            klog("ext2: %s is a \"myos-data\" of another disk: left alone\n", data_seen[k]->name);
    /* The config Settings saved: on the root's disk, so a live stick never
       runs an installed system's tasks (nor the reverse). With no root
       disk, the first good one. Before sched_start(), which uses it. */
    kconfig_from_disk(rootdev ? disk_of_part(rootdev) : NULL);
    if (rootdev)
        mount_root(rootdev);
    if (datadev)
        ext2_mount(datadev, "/data");

    /* The first FAT32 partition (the test stick, a USB disk) at /mnt.
       Read-only until FAT writing exists (step 9e). */
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_get(i);
        if (!d->parent || (partition_type(d) != 0x0c && partition_type(d) != 0x0b))
            continue;
        if (fat_vfs_mount(d, "/mnt") == 0)
            break;
    }
}

/* Did a UEFI loader start us? Then it has printed the title and Step 01 on
   the firmware's console, which is the serial port too in QEMU. (The 32-bit
   kernel's own BIOS loader prints nothing on serial.) */
static int uefi_loader(void)
{
#ifdef ARCH_X86_64
    return 1;                           /* the only way it starts */
#else
    return ((const struct bootinfo *)P2V(BOOTINFO_PHYS))->fb_magic == BOOTINFO_FB_MAGIC;
#endif
}

void kernel_main(void)
{
#ifdef BENCH
    uint64_t boot_start = cycles_now();     /* for the boot-time benchmark */
#endif
    serial_init();
    vga_text_init();        /* the console on the BIOS's 80x25 text screen */
    vt_clear();
    /* The screen gets one line per stage (include/kprintf.h); what each
       stage finds goes to serial with klog, indented under it. */
    boot_title(!uefi_loader());
#ifdef ARCH_X86_64
    klog("Hello world from MyOS!  (long mode x86_64, UEFI loader)\n");
#else
    klog("Hello world from MyOS!  (protected mode, %s)\n",
         uefi_loader() ? "UEFI loader" : "own boot loader");
#endif
    klog("Open source for your enjoyment.\n");
    klog("serial: %s\n", serial_present() ? "COM1" : "no COM1 (messages on the screen only)");

    boot_step("CPU");
    cpu_init();             /* what can this CPU do? (stops if too old) */
    gdt_init();             /* our own segment table + TSS */
    idt_init();             /* interrupt table + exception handlers + PIC remap */
    serial_irq_init();      /* serial output now queued, sent by IRQ 4 */
    fpu_init();             /* x87 + SSE on; saved per user thread */

    boot_step("Memory");
    pmm_init();             /* which physical RAM is free (BIOS memory map) */
    vmm_init();             /* map all RAM with 4 MiB pages; unmap NULL */
    pmm_refs_init();        /* pages may now be shared (copy-on-write) */
    mm_init();              /* address spaces */
    klog("Memory: %lu MiB usable, paging on, kernel at %p, heap at %p\n",
         (unsigned long)(pmm_usable_count() / 256), (void *)_start, (void *)KHEAP_START);
    sysenter_init();        /* fast syscalls, if the CPU has sysenter */

    boot_step("Timer and input");
    timer_init();           /* IRQ 0: 1 ms tick */
    if (keyboard_init() == 0)   /* IRQ 1, if the PC has a PS/2 controller */
        mouse_init();           /* IRQ 12, if there is a PS/2 mouse */
    interrupts_enable();    /* sti: from now on, IRQs can arrive */
    timer_check();          /* a PIT that never ticks (new chipsets): the HPET */

    boot_step("Clock and randomness");
    time_init();            /* the date and time, from the CMOS clock */
    random_init();          /* measure boot entropy, key the CSPRNG: before
                               anything that needs an unguessable number */
    boot_step("Boot files");
    initrd_init();          /* the initramfs stage 2 loaded */
    boot_step("Threads");
    thread_init();          /* kernel_main becomes thread 0, "main" */
    shim_init();            /* Linux's timers and work queues, for drivers */
    boot_step("PCI devices");
    pci_init();             /* which PCI devices are fitted: the IDE controller, the NIC */
    acpi_init();            /* the firmware's power-off numbers (before any process) */
    boot_step("Graphics");
    drm_init();             /* a display card: the console moves to pixels, /dev/fb0 */
    boot_step("Sound");
    hda_intel_init();       /* HD Audio: the sound card, /dev/snd (before the
                               root disk: its nodes go in the boot /dev) */
    storage_init();         /* "Disks and USB", then "File systems" */
    boot_step("Network");
    net_init();             /* loopback, then the e1000 and IPv4 if the card is there */
#if defined(WIFI_SIM) && !defined(SELFTEST)
    {   /* make WIFISIM=1: the self-tests' simulated radio, for trying the
           Wi-Fi programs on a whole system (test builds only). */
        int sim_wifi_init(void);
        boot_step("Wi-Fi simulator (debug mode)");
        if (sim_wifi_init() == 0)
            klog("wifi: the simulated radio is up (MyOS-Test, MyOS-Open-5G)\n");
    }
#endif
#ifdef MYOS_IWLWIFI
    {   /* Intel Wi-Fi (Linux's iwlwifi, built in; drivers/net/wireless/
           intel/iwlwifi/myos.c). The firmware loads in the background from
           /lib/firmware (the root file system is mounted by now). */
        void iwlwifi_myos_init(void);
#ifdef MYOS_IWLDEBUG
        boot_step("Wi-Fi (debug mode)");
        iwlwifi_myos_init();
        klog("wifi: iwlwifi built in, with its debug lines (dmesg)\n");
#else
        boot_step("Wi-Fi");
        iwlwifi_myos_init();
        klog("wifi: iwlwifi built in (Intel Wi-Fi 6 AX201 and its family)\n");
#endif
    }
#endif

#if defined(SELFTEST)
    boot_step("Self-tests (debug mode)");
    run_selftests();        /* ends with the slot scheduler running */
    boot_end();
    kprintf("Type here (F10/F11/F12 = test a CPU fault)> ");
#elif defined(BENCH)
    boot_step("Benchmarks (debug mode)");
    run_benchmarks(boot_start);     /* switches QEMU off when done */
#else
    boot_step("Starting programs");
    boot_end();
    sched_start();          /* the 20 x 1 ms slot scheduler */
    run_init();             /* the first program, if the initramfs has one */
    kprintf("Type here> ");
#endif

    /* Main loop: sleep until an interrupt wakes us (timer tick or key),
       then handle whatever is new. main owns the system slots 1 and 10. */
    uint32_t last_secs = 0xFFFFFFFF;
    for (;;) {
        cpu_wait();
        net_rx_action();            /* packets queued by a NIC interrupt */

        uint32_t secs = timer_ms() / 1000;
        if (secs != last_secs) {
            show_uptime(secs);
            last_secs = secs;
        }

        int c;
        while ((c = keyboard_getc()) != -1) {
#ifdef SELFTEST
            if (selftest_key(c))    /* F10/F11/F12: crash on purpose */
                continue;
#endif
            echo(c);
        }
#ifdef SELFTEST
        /* make test moves and clicks the mouse through QEMU's monitor and
           looks for these lines. */
        struct mouse_event m;
        while (mouse_get(&m) == 0)
            kprintf("\nmouse: dx=%d dy=%d wheel=%d buttons=%u\n",
                    m.dx, m.dy, m.wheel, (unsigned)m.buttons);
#endif
    }
}
