; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; =============================================================================
; entry.asm — first kernel code: turn on paging, move to the higher half
; =============================================================================
; Stage 2 loads the kernel at PHYSICAL 0x100000 (1 MiB) and jumps there. But
; the kernel is linked to run at VIRTUAL 0xC0100000 (see linker.ld and
; memlayout.h). So this code starts running at an address that differs
; from the one the linker assumed, and must be careful:
;
;   until paging is on, every label must be used as (label - KERNEL_VBASE)
;
; The plan:
;   1. clear .bss
;   2. map the first 4 MiB of RAM TWICE in the page directory:
;        virtual 0x00000000-0x003FFFFF -> physical 0-4 MiB  ("identity map")
;        virtual 0xC0000000-0xC03FFFFF -> physical 0-4 MiB  (the higher half)
;      The identity map is needed because the instruction right after
;      "paging on" is still fetched from the low address we are running at.
;      We use one 4 MiB "big page" for each (PSE, every Pentium and later
;      has it), so no page table is needed at all.
;   3. turn paging on, jump to the higher-half address
;   4. switch to our own stack and call kernel_main()
; The C code (paging.c) later maps all of RAM and removes the identity map.
;
; How x86 paging translates a 32-bit virtual address:
;   bits 31-22: index into the PAGE DIRECTORY (1024 entries, each covers 4 MiB)
;   bits 21-12: index into that entry's PAGE TABLE (1024 entries, 4 KiB each)
;   bits 11-0 : offset inside the 4 KiB page
; CR3 holds the physical address of the page directory.
; With PSE (CR4 bit 4) on, a directory entry with the PS bit (0x80) set maps
; a whole 4 MiB page directly, skipping the page table level.
; =============================================================================

[bits 32]

KERNEL_VBASE equ 0xC0000000         ; keep in sync with memlayout.h
PTE_PRESENT  equ 0x001
PTE_WRITABLE equ 0x002
PTE_PS       equ 0x080              ; directory entry maps a 4 MiB page
CR4_PSE      equ 0x010              ; enable 4 MiB pages

section .text.entry
global _start
extern kernel_main
extern __bss_start, __bss_end       ; defined in linker.ld

_start:
    ; --- 1. Zero .bss (the MBR only copied the bytes stored in kernel.bin) --
    cld
    mov edi, __bss_start - KERNEL_VBASE
    mov ecx, __bss_end - KERNEL_VBASE
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; --- 2. One 4 MiB page -> physical 0..4 MiB, at 0 and at 0xC0000000 ------
    mov eax, cr4
    or eax, CR4_PSE
    mov cr4, eax

    mov eax, 0 | PTE_PRESENT | PTE_WRITABLE | PTE_PS    ; physical 0, 4 MiB
    mov [boot_page_directory - KERNEL_VBASE + 0 * 4], eax    ; 0x00000000
    mov [boot_page_directory - KERNEL_VBASE + 768 * 4], eax  ; 0xC0000000 (768 * 4 MiB)

    ; --- 3. Paging on ---------------------------------------------------------
    mov eax, boot_page_directory - KERNEL_VBASE
    mov cr3, eax                        ; where the page directory is (physical)
    mov eax, cr0
    or eax, 0x80000000                  ; CR0 bit 31 = PG (paging enable)
    mov cr0, eax

    ; Still running at the low address (thanks to the identity map).
    ; Jump to the real, high address of the next instruction:
    mov eax, higher_half
    jmp eax

higher_half:
    ; --- 4. From here on, addresses are what the linker expected ------------
    mov esp, boot_stack_top
    call kernel_main

.hang:                                  ; kernel_main should never return
    cli
    hlt
    jmp .hang

; -----------------------------------------------------------------------------
section .bss nobits alloc noexec write align=4096

global boot_page_directory
alignb 4096
boot_page_directory:    resb 4096       ; must be 4 KiB aligned (CR3 needs it)
boot_stack:             resb 16384      ; 16 KiB kernel stack
global boot_stack_top                   ; (a crash's stack scan: where "main"'s stack ends)
boot_stack_top:
