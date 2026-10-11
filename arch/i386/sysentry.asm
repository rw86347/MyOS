; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; =============================================================================
; sysentry.asm — fast system calls: sysenter / sysexit
; =============================================================================
; `int 0x80` goes through the whole interrupt machinery: look up the IDT,
; check privileges, switch stacks via the TSS, push five values... On a
; Pentium 4 that costs well over a thousand cycles. `sysenter` (Pentium II and
; later) is a stripped-down door into the kernel: it loads CS, SS, ESP and EIP
; from three model-specific registers (MSRs) and nothing else. `sysexit` is
; the matching way out.
;
; Because sysenter saves nothing — not even where to return to — Linux puts a
; tiny piece of code in every program's memory (the "vsyscall" page) and
; tells the program its address in the auxiliary vector (AT_SYSINFO). musl
; and glibc make every system call through it:
;
;   __kernel_vsyscall:          (user mode, in the vsyscall page)
;       push ecx                save what sysexit will overwrite
;       push edx
;       push ebp                arg 6 goes on the stack: EBP is needed
;       mov ebp, esp            to carry the user stack pointer
;       sysenter          --->  sysenter_entry (kernel, below)
;   .ret:                 <---  sysexit returns here (EDX), stack = ECX
;       pop ebp
;       pop edx
;       pop ecx
;       ret
;
; The kernel side builds exactly the same frame (struct regs) that `int 0x80`
; would, so all the C code is shared.
; =============================================================================

[bits 32]

GDT_USER_CODE   equ 0x1B
GDT_USER_DATA   equ 0x23
GDT_KERNEL_DATA equ 0x10
VSYSCALL_ADDR   equ 0xBFFFF000      ; keep in sync with sysenter.h

extern kernel_tss
extern sysenter_dispatch
extern isr_return

; ---------------------------------------------------------------------------
; The vsyscall page's code. Copied into a page at boot (sysenter.c) and
; mapped at VSYSCALL_ADDR, read-only, in every program.
section .rodata
global vsyscall_code, vsyscall_code_end
vsyscall_code:
    push ecx
    push edx
    push ebp
    mov ebp, esp
    sysenter
.ret:
    pop ebp
    pop edx
    pop ecx
    ret
vsyscall_code_end:

VSYSCALL_RET equ VSYSCALL_ADDR + (vsyscall_code.ret - vsyscall_code)

; ---------------------------------------------------------------------------
section .text
global sysenter_entry
sysenter_entry:
    ; The CPU loaded CS = kernel code, SS = kernel data, and ESP from an MSR
    ; (a small spare stack, only used for the next instruction). Interrupts
    ; are off. The user's stack pointer is in EBP.
    mov esp, [kernel_tss + 4]       ; this thread's kernel stack (TSS.esp0)

    ; Build the frame an `int 0x80` from ring 3 would have left:
    push GDT_USER_DATA              ; user SS
    push ebp                        ; user ESP
    pushfd                          ; user EFLAGS (sysenter only cleared IF)
    or dword [esp], 0x200           ;   user code always runs with IF on
    push 2                          ; ...and the kernel starts with clean flags
    popfd                           ;   (no stray direction/trap/NT flag)
    push GDT_USER_CODE              ; user CS
    push VSYSCALL_RET               ; user EIP: back into the vsyscall page
    push 0                          ; error code  } as the isr.asm stubs push
    push 0x80                       ; vector      }
    pushad
    push ds
    push es
    push fs
    push gs
    mov ax, GDT_KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                        ; argument: the frame (struct regs *)
    call sysenter_dispatch
    add esp, 4

    ; sysexit can't restore ECX and EDX (it uses them), which is fine for
    ; an ordinary syscall: the vsyscall code saved them. But if the return
    ; address changed — a signal handler to run, sigreturn, execve — every
    ; register must come back exactly: take the full `iret` way instead.
    cmp dword [esp + 56], VSYSCALL_RET  ; the frame's EIP
    jne isr_return

    pop gs
    pop fs
    pop es
    pop ds
    popad                           ; EAX = the syscall's result
    add esp, 8                      ; drop vector and error code
    ; sysexit: EIP <- EDX, ESP <- ECX, CS/SS <- the user selectors.
    mov edx, [esp]                  ; user EIP
    mov ecx, [esp + 12]             ; user ESP
    sti                             ; takes effect after the NEXT instruction,
    sysexit                         ;   so no interrupt can arrive in between
