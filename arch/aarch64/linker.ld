/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * linker.ld — minimal aarch64 kernel for QEMU "virt"
 *
 * QEMU -kernel loads ELF at the link address. DTB from -dtb sits at 0x40000000
 * (up to 1 MiB in dumpdtb); link the kernel at 0x40100000 to avoid overlap.
 * MMU is off at entry until paging (14a).
 *
 * Keep .bss and page tables in the first PT_LOAD (with .text/.rodata) so every
 * writable kernel symbol is in the image region QEMU maps before MMU enable.
 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x40100000;

    .vectors : ALIGN(2048) {
        KEEP(*(.text.vectors))
    }

    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata.dtb)
        *(.rodata .rodata.*)
    }

    .pagetables : ALIGN(4096) {
        *(.pagetables)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;

    /* EL0 bring-up: code + rodata at a fixed user VA (see uaccess.h). */
    .user 0x40200000 : {
        __user_start = .;
        *(.user.text)
        *(.user.rodata)
        __user_end = .;
    }

    /* Kernel stack (fixed; not in .bss). */
    . = ALIGN(16);
    . = 0x40260000;
    __stack_top = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
