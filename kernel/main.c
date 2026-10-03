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
#include <bcache.h>
#include <blockdev.h>
#include <cpu.h>
#include <ext2.h>
#include <string.h>
#include <cycles.h>
#include <fpu.h>
#include <gdt.h>
#include <initrd.h>
#include <interrupts.h>
#include <kheap.h>
#include <keyboard.h>
#include <kprintf.h>
#include <memlayout.h>
#include <mouse.h>
#include <mm.h>
#include <proc.h>
#include <vfs.h>
#include <paging.h>
#include <partition.h>
#include <pmm.h>
#include <sched.h>
#include <serial.h>
#include <sysenter.h>
#include <thread.h>
#include <syscalls.h>
#include <timer.h>
#include <tty.h>
#include <vga.h>
#ifdef SELFTEST
#include <selftest.h>
#endif
#ifdef BENCH
#include <bench.h>
#endif

extern char _start[];       /* first byte of the kernel (entry.asm) */

#if !defined(SELFTEST) && !defined(BENCH)
/* Start /init from the initramfs as process 1, like Linux does, and wait
   for it (main sleeps, so its system slots go to the programs). Returns
   only if there is no /init. */
static void run_init(void)
{
    static const char *const argv[] = { "/init", NULL };
    static const char *const envp[] = {
        "HOME=/", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "TERM=linux", NULL,
    };
    struct inode *ino;
    if (vfs_lookup("/init", &ino) < 0)
        return;                         /* nothing to run: stay in the kernel */
    struct process *p = proc_spawn("/init", argv, envp);
    if (!p) {
        kprintf("Could not start /init\n");
        return;
    }
    proc_set_init(p);
    tty_set_foreground(p->pgid);        /* Ctrl-C reaches init's group */
    tty_attach_keyboard();              /* keys go to the terminal now */
    int status = proc_wait_kernel(p);
    kprintf("\n/init exited (status %d). System halted.\n", status);
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
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
    vga_write_at(80 - sizeof s + 1, 0, s, VGA_BLACK, VGA_LIGHT_GREY);
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

/* Disks: find them and their partitions, then our own file systems on
   them, by label — on a Linux partition (type 0x83) or a whole disk with
   no partition table:
     "myos-root"  the whole system: becomes "/" (an installed MyOS)
     "myos-data"  becomes /data
   Without a "myos-root", MyOS runs from the initramfs in RAM (a live
   disk). Anything else is left alone: an old PC may still hold someone's
   Linux disk, and we never write to that. */
static void storage_init(void)
{
    ata_init();
    int ndisks = blockdev_count();      /* scanning adds the partitions */
    for (int i = 0; i < ndisks; i++)
        partition_scan(blockdev_get(i));
    bcache_init();                      /* and their blocks, cached */

    struct blockdev *rootdev = NULL, *datadev = NULL;
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_get(i);
        char label[17];
        if (d->parent ? partition_type(d) != 0x83 : has_partitions(d))
            continue;
        if (ext2_label(d, label) < 0)
            continue;                   /* not ext2 */
        if (!rootdev && strcmp(label, "myos-root") == 0)
            rootdev = d;
        else if (!datadev && strcmp(label, "myos-data") == 0)
            datadev = d;
        else
            kprintf("ext2: %s (\"%s\") is not a MyOS disk: left alone\n", d->name, label);
    }
    if (rootdev)
        mount_root(rootdev);
    if (datadev)
        ext2_mount(datadev, "/data");
}

void kernel_main(void)
{
#ifdef BENCH
    uint64_t boot_start = cycles_now();     /* for the boot-time benchmark */
#endif
    serial_init();
    vga_clear();
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("Hello world from MyOS!  (protected mode, own boot loader)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    cpu_init();             /* what can this CPU do? (stops if too old) */

    /* CPU tables and interrupts. */
    gdt_init();             /* our own segment table + TSS */
    idt_init();             /* interrupt table + exception handlers + PIC remap */
    serial_irq_init();      /* serial output now queued, sent by IRQ 4 */
    fpu_init();             /* x87 + SSE on; saved per user thread */

    /* Memory. */
    pmm_init();             /* which physical RAM is free (BIOS memory map) */
    vmm_init();             /* map all RAM with 4 MiB pages; unmap NULL */
    pmm_refs_init();        /* pages may now be shared (copy-on-write) */
    mm_init();              /* address spaces */
    sysenter_init();        /* fast syscalls, if the CPU has sysenter */
    timer_init();           /* IRQ 0: 1 ms tick */
    keyboard_init();        /* IRQ 1 */
    mouse_init();           /* IRQ 12, if there is a PS/2 mouse */
    interrupts_enable();    /* sti: from now on, IRQs can arrive */

    kprintf("Memory: %lu MiB usable, paging on, kernel at %p, heap at %p\n",
            pmm_usable_count() / 256, (void *)_start, (void *)KHEAP_START);

    time_init();            /* the date and time, from the CMOS clock */
    initrd_init();          /* the initramfs stage 2 loaded */
    thread_init();          /* kernel_main becomes thread 0, "main" */
    storage_init();         /* IDE disks and their partitions */

#if defined(SELFTEST)
    run_selftests();        /* ends with the slot scheduler running */
    kprintf("Type here (F10/F11/F12 = test a CPU fault)> ");
#elif defined(BENCH)
    run_benchmarks(boot_start);     /* switches QEMU off when done */
#else
    sched_start();          /* the 20 x 1 ms slot scheduler */
    run_init();             /* the first program, if the initramfs has one */
    kprintf("Type here> ");
#endif

    /* Main loop: sleep until an interrupt wakes us (timer tick or key),
       then handle whatever is new. main owns the system slots 1 and 10. */
    uint32_t last_secs = 0xFFFFFFFF;
    for (;;) {
        cpu_wait();

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
