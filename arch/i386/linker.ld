/*
 * linker.ld — memory layout of the kernel image
 *
 * The MBR copies kernel.bin from disk to physical address 0x10000 and jumps
 * there, so we tell the linker the kernel *runs* at 0x10000 (". = 0x10000").
 * Every absolute address in the kernel (string literals, globals, calls) is
 * computed relative to that.
 *
 * .text.entry comes first so _start is the very first byte of kernel.bin.
 * Keep in sync with KERNEL_LOAD in arch/i386/boot/mbr.asm.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text.entry)          /* _start — must be first */
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)    /* constant data, e.g. string literals */
    }

    .data : {
        *(.data .data.*)        /* initialized globals */
    }

    /* .bss holds zero-initialized globals. It takes no space in kernel.bin;
       entry.asm clears it at boot using these two symbols. */
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
