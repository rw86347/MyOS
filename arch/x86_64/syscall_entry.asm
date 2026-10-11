; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; syscall_entry.asm — the `syscall` instruction lands here (MSR LSTAR)
;
; What `syscall` does: RCX = the user's RIP, R11 = the user's RFLAGS, CS and
; SS from MSR STAR, RFLAGS &= ~SFMASK (so IF = 0: interrupts off, as for
; int 0x80 on i386) — and nothing to the stack: RSP is still the program's.
; So first we move to this thread's kernel stack (TSS.rsp0, set at every
; thread switch), keeping the user RSP in a scratch word. One CPU, and
; interrupts are off, so nothing else can use the word in between. (With
; several CPUs it would be per-CPU, reached with swapgs.)
;
; Then we build exactly the frame an interrupt builds (isr.asm), so that
; struct regs, fork, execve and signals see one layout:
;
;   ss rsp rflags cs rip | error vector | rax rcx rdx rbx rbp rsi rdi r8-r15
;
; Back out with sysret (fast: no stack reads by the CPU) when it gives
; exactly what the frame says. sysret puts RIP in RCX and RFLAGS in R11, so
; that is when the frame's RCX == RIP and R11 == RFLAGS — true after any
; ordinary syscall, which is why Linux defines both as clobbered. Otherwise
; (execve, a signal handler, sigreturn changed the frame) iretq, like an
; interrupt. Also iretq if RIP isn't a user address: on Intel CPUs, sysret
; to a non-canonical RIP faults while still in ring 0 but on the USER stack
; — a well-known way into the kernel.

[bits 64]
default rel

extern syscall_entry_c
extern kernel_tss
extern isr_return

GDT_USER_CODE equ 0x2B
GDT_USER_DATA equ 0x23
TSS_RSP0      equ 4                 ; offset of rsp0 in the 64-bit TSS
USER_ADDR_END equ 0x00007FFFFFFFF000

; struct regs offsets (interrupts.h)
R_R11    equ 32
R_RCX    equ 104
R_RIP    equ 136
R_CS     equ 144
R_RFLAGS equ 152
R_RSP    equ 160

section .bss
user_rsp: resq 1

section .text
global syscall_entry_asm
syscall_entry_asm:
    mov [user_rsp], rsp
    mov rsp, [kernel_tss + TSS_RSP0]

    push GDT_USER_DATA              ; ss
    push qword [user_rsp]           ; rsp
    push r11                        ; rflags
    push GDT_USER_CODE              ; cs
    push rcx                        ; rip
    push 0                          ; error code  } as the isr.asm stubs push
    push 0x80                       ; vector      } (0x80: "a system call")
    push rax                        ; the same order as isr_common
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                    ; argument: the frame (struct regs *)
    call syscall_entry_c

    mov rcx, [rsp + R_RIP]
    cmp rcx, [rsp + R_RCX]
    jne isr_return
    mov r11, [rsp + R_RFLAGS]
    cmp r11, [rsp + R_R11]
    jne isr_return
    cmp qword [rsp + R_CS], GDT_USER_CODE
    jne isr_return
    mov rax, USER_ADDR_END
    cmp rcx, rax
    jae isr_return

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11                         ; == RFLAGS (checked above)
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx                         ; == RIP
    pop rax                         ; the syscall's result
    mov rsp, [rsp + R_RSP - 120]    ; (15 registers popped) the user's stack
    o64 sysret                      ; (NASM's spelling of sysretq)
