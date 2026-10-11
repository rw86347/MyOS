; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; =============================================================================
; mbr.asm — MyOS Master Boot Record (stage 1 bootloader)
; =============================================================================
;
; What happens at power-on:
;   1. The BIOS runs its self-test, then reads the FIRST 512-byte sector of the
;      boot disk (the "Master Boot Record") into memory at address 0x7C00.
;   2. It checks that the last two bytes are 0x55 0xAA (the "boot signature").
;   3. It jumps to 0x7C00 with the CPU in 16-bit "real mode" — the same mode an
;      8086 from 1978 ran in. DL holds the BIOS drive number we booted from.
;
; 512 bytes (446, really — the rest is the partition table) is too small for
; a real loader. So this is "stage 1": it only loads stage 2 (the sectors
; right after it) and jumps there. Stage 2 has room to do the real work:
; memory map, loading the kernel and initramfs above 1 MiB, protected mode.
;
; Disk layout of build/myos.img:
;   sector 0 (LBA 0)                this MBR
;   LBA 1 .. STAGE2_SECTORS         stage 2 (stage2.asm)
;   next                            the boot picture (stage 2 loads it; not us)
;   next                            kernel.bin
;   next                            initrd.cpio (the initramfs)
; =============================================================================

[bits 16]                       ; the CPU starts in 16-bit real mode
[org 0x7C00]                    ; the BIOS loads us here, so addresses start here

%include "layout.inc"

start:
    ; --- Segments and stack ---------------------------------------------------
    ; In real mode an address is segment*16 + offset. Some BIOSes jump to us as
    ; 0x07C0:0x0000 instead of 0x0000:0x7C00, so the far jump below forces CS=0.
    jmp 0x0000:.flush_cs
.flush_cs:
    cli                         ; no interrupts while we change the stack
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stack grows down from just below our code
    sti

    mov [boot_drive], dl        ; remember which drive the BIOS booted us from

    mov si, msg_loading
    call print16

    ; --- Load stage 2 with BIOS int 0x13, function 0x42 ("extended read") ------
    ; It takes a Disk Address Packet (DAP) that says how many sectors to read,
    ; from which LBA (sector number), into which segment:offset. LBA addressing
    ; is much simpler than the old cylinder/head/sector scheme and is supported
    ; by BIOSes from ~1998 on.
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error               ; carry flag set = the read failed

    mov dl, [boot_drive]        ; stage 2 wants the drive number in DL too
    jmp 0x0000:STAGE2_LOAD

disk_error:
    mov si, msg_disk_error
    call print16
.hang:
    hlt
    jmp .hang

; print16: print the zero-terminated string at DS:SI using the BIOS (real mode only)
print16:
    mov ah, 0x0E                ; BIOS teletype output
.next:
    lodsb                       ; al = [si], si++
    test al, al
    jz .done
    int 0x10
    jmp .next
.done:
    ret

; -----------------------------------------------------------------------------
; Data
; -----------------------------------------------------------------------------
align 4
dap:                            ; Disk Address Packet for int 0x13 / ah=0x42
    db 0x10                     ; size of this packet (16 bytes)
    db 0                        ; reserved
    dw STAGE2_SECTORS           ; number of sectors to read
    dw STAGE2_LOAD              ; destination offset  } 0x0000:0x7E00
    dw 0                        ; destination segment }
    dq 1                        ; starting LBA (sector 1, right after the MBR)

boot_drive:     db 0
msg_loading:    db "MyOS: stage 1", 13, 10, 0
msg_disk_error: db "MyOS: disk read error!", 13, 10, 0

; -----------------------------------------------------------------------------
; Pad to 510 bytes and add the boot signature
; -----------------------------------------------------------------------------
; Bytes 446-509 are where a partition table lives on a real disk. We don't
; need one for QEMU yet, but keep the space free: if the code above grows
; past 446 bytes, "times" gets a negative count and nasm stops with an error.
times 446 - ($ - $$) db 0
times 64 db 0                   ; partition table (4 empty entries)
dw 0xAA55                       ; stored little-endian as 0x55, 0xAA
