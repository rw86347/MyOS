/*
 * linker.ld — memory layout of the kernel image
 *
 * Two addresses for everything:
 *   VMA (virtual)  where the code RUNS once paging is on:  0xC0100000...
 *   LMA (load)     where stage 2 PUTS it in physical RAM:  0x100000 (1 MiB)
 * "AT(...)" sets the load address. objcopy -O binary lays the file out by
 * load address, so kernel.bin starts with _start, loaded at 1 MiB.
 *
 * The kernel, including .bss, must end below 4 MiB: that's all entry.asm
 * maps before paging.c sets up the rest.
 *
 * Keep in sync with KERNEL_LOAD in boot/layout.inc and KERNEL_VBASE in
 * memlayout.h and entry.asm.
 */
ENTRY(_start)

KERNEL_VBASE = 0xC0000000;

SECTIONS
{
    . = KERNEL_VBASE + 0x100000;

    .text : AT(ADDR(.text) - KERNEL_VBASE) {
        *(.text.entry)          /* _start — must be first */
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VBASE) {
        *(.rodata .rodata.*)    /* constant data, e.g. string literals */
        . = ALIGN(4);
        __start_ex_table = .;   /* uaccess.asm's (faulting insn, fixup) pairs */
        *(__ex_table)
        __stop_ex_table = .;
    }

    .data : AT(ADDR(.data) - KERNEL_VBASE) {
        *(.data .data.*)        /* initialized globals */
    }

    /* .bss holds zero-initialized globals. It takes no space in kernel.bin;
       entry.asm clears it at boot using these two symbols. */
    .bss : AT(ADDR(.bss) - KERNEL_VBASE) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;           /* first address after the kernel (virtual) */

    /* entry.asm maps only the first 4 MiB before paging.c maps the rest:
       a kernel (with its zeroed data) bigger than that must fail the build,
       not the boot. */
    ASSERT(__kernel_end - KERNEL_VBASE <= 0x400000,
           "kernel + .bss must end below 4 MiB (entry.asm maps only that)")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
