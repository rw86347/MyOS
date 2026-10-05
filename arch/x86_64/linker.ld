/*
 * linker.ld — first x86_64 kernel image
 *
 * Linked to run at physical 1 MiB (VMA = LMA). The UEFI loader copies
 * kernel.bin there and jumps to _start in long mode. No higher half yet:
 * that waits for our own page tables.
 */
ENTRY(_start)

KERNEL_LOAD = 0x100000;

SECTIONS
{
    . = KERNEL_LOAD;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(4);
        __start_ex_table = .;
        *(__ex_table)
        __stop_ex_table = .;
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

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
