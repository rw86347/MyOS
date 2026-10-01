; =============================================================================
; layout.inc — addresses shared by mbr.asm and stage2.asm
; =============================================================================
; Keep in sync with: bootinfo.h (BOOTINFO layout), linker.ld (KERNEL_LOAD),
; and the Makefile (STAGE2_SECTORS, passed in with -D).
;
; Memory while booting (real mode can only reach the first 1 MiB):
;
;   0x00001000  BOOTINFO: what stage 2 leaves for the kernel (memory map,
;               where the initramfs is)
;   0x00007C00  the MBR (the BIOS puts it here); the stack grows down below it
;   0x00007E00  stage 2 (right after the MBR)
;   0x00010000  bounce buffer: disk reads land here first (64 KiB)
;   0x00100000  the kernel (1 MiB, like Linux), then the initramfs after it
; =============================================================================

%ifndef STAGE2_SECTORS
  %error "STAGE2_SECTORS must be defined (the Makefile does this)"
%endif

BOOTINFO        equ 0x1000
E820_MAX        equ 32
STAGE2_LOAD     equ 0x7E00
BOUNCE          equ 0x10000
CHUNK_SECTORS   equ 64              ; 32 KiB per BIOS disk read
KERNEL_LOAD     equ 0x100000
