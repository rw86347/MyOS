; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; efi_jump.asm — copied to physical 0x3000, then jumped to with paging on.
; Drops from 64-bit long mode (what UEFI left us in) to 32-bit protected
; mode with no paging, the state our kernel's entry.asm expects.
;
; Why a low-memory copy: the EFI app itself is often loaded above 4 GiB.
; After paging is off, a 32-bit jump can only reach the first 4 GiB, so
; the last instructions have to live down here.
;
; Also: turn the local APIC off. Firmware may have it on; our kernel
; still talks to the old PIC. SMEP/SMAP would break copy_from_user.

[bits 64]
[org 0x3000]

start:
    cli
    ; Firmware on a modern laptop is often in x2APIC. Clearing only the
    ; global-enable bit then #GPs; both bits have to go in one WRMSR.
    mov ecx, 0x1B                   ; IA32_APIC_BASE
    rdmsr
    and eax, ~((1 << 11) | (1 << 10))
    wrmsr

    lgdt [abs gdt_ptr]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    push 0x08                       ; 32-bit code selector
    push compat32
    retfq

[bits 32]
compat32:
    ; PCIDE/LA57/SMEP/SMAP must be cleared *before* PG: writing CR0.PG=0
    ; with CR4.PCIDE=1 is a #GP on Intel (MSI boxes often have it on).
    mov eax, cr4
    and eax, ~((1 << 12) | (1 << 17) | (1 << 20) | (1 << 21))
    mov cr4, eax

    mov eax, cr0
    and eax, 0x7FFFFFFF             ; PG off: long mode ends, we are PE-only
    mov cr0, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    and eax, ~((1 << 8) | (1 << 11)) ; LME, NXE
    wrmsr

    mov eax, cr4
    and eax, ~(1 << 5)              ; PAE
    or  eax, (1 << 4)               ; PSE: entry.asm wants 4 MiB pages
    mov cr4, eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000                ; a scratch stack; the kernel replaces it

    mov eax, 0x100000               ; KERNEL_LOAD
    jmp eax

align 16
gdt:
    dq 0
    dw 0xFFFF, 0, 0x9A00, 0x00CF    ; 0x08: 32-bit code, 0..4GiB
    dw 0xFFFF, 0, 0x9200, 0x00CF    ; 0x10: 32-bit data, 0..4GiB
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dq gdt
