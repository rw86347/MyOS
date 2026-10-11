; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; =============================================================================
; usermode.asm — drop from the kernel (ring 0) into a user program (ring 3)
; =============================================================================
; There is no "go to ring 3" instruction. But there IS a way back from an
; interrupt that came from ring 3: `iret` pops EIP, CS, EFLAGS, ESP and SS,
; and if the new CS has privilege level 3, the CPU switches to ring 3.
;
; So we push exactly what the CPU would have pushed if this program had been
; interrupted at its first instruction, and "return" to it:
;
;     | SS     |  user data segment (0x23)
;     | ESP    |  the program's stack
;     | EFLAGS |  with IF set: interrupts stay on in user mode
;     | CS     |  user code segment (0x1B)
;     | EIP    |  the program's first instruction      <- ESP before iret
; =============================================================================

[bits 32]
GDT_USER_CODE equ 0x1B
GDT_USER_DATA equ 0x23

section .text

; void user_enter(uint32_t eip, uint32_t esp);   — never returns
global user_enter
user_enter:
    mov ecx, [esp + 4]          ; eip
    mov edx, [esp + 8]          ; esp

    mov ax, GDT_USER_DATA       ; data segments for the program. (The kernel
    mov ds, ax                  ;  still works with these until iret: they're
    mov es, ax                  ;  flat too. Any interrupt reloads 0x10.)
    mov fs, ax
    mov gs, ax

    push GDT_USER_DATA          ; SS
    push edx                    ; ESP
    pushfd                      ; EFLAGS ...
    or dword [esp], 0x200       ; ... with IF (bit 9) on
    push GDT_USER_CODE          ; CS
    push ecx                    ; EIP
    iretd                       ; and we're in ring 3
