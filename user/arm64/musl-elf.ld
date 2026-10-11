/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * Static musl aarch64 link address in QEMU virt RAM (not 0x400000 — that maps to PA 0).
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
  . = 0x40400000;
  . = ALIGN(65536);

  .text : ALIGN(65536) {
    KEEP(*(.init))
    *(.text .text.*)
    KEEP(*(.fini))
  }

  .rodata : {
    *(.rodata .rodata.*)
  }

  .data : {
    *(.data .data.*)
    *(.got .got.plt)
  }

  .bss : {
    __bss_start = .;
    *(COMMON)
    *(.bss .bss.*)
    __bss_end = .;
  }

  /DISCARD/ : {
    *(.note*)
    *(.comment)
    *(.eh_frame*)
  }
}
