/*
 * linker.ld — the x86_64 kernel image
 *
 * Two addresses per section, as on i386:
 *   VMA (where the code RUNS):     KERNEL_VBASE + 1 MiB, the top 2 GiB
 *   LMA (where it is LOADED):      physical 1 MiB
 * The UEFI loader copies kernel.bin (objcopy writes it by LMA) to 1 MiB and
 * jumps there; entry.asm maps the top half and jumps up (memlayout.h).
 */
ENTRY(_start)

KERNEL_LOAD  = 0x100000;
KERNEL_VBASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VBASE + KERNEL_LOAD;

    .text : AT(ADDR(.text) - KERNEL_VBASE) {
        __text_start = .;       /* (a crash's stack scan: what is code) */
        *(.text.entry)
        *(.text .text.*)
        __text_end = .;
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VBASE) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __start_ex_table = .;
        *(__ex_table)
        __stop_ex_table = .;
    }

    .data : AT(ADDR(.data) - KERNEL_VBASE) {
        *(.data .data.*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VBASE) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;
    /* The same, physical: the Makefile gives it to the UEFI loader. */
    __kernel_end_phys = . - KERNEL_VBASE;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
