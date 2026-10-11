; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; switch.asm — x86_64 kernel thread switch (cdecl64: rdi, rsi)
[bits 64]

extern thread_trampoline
extern thread_exit

global switch_context
switch_context:
    mov rax, rdi
    mov rdx, rsi
    push rbp
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    mov [rax], rsp
    mov rsp, rdx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    ret

global thread_trampoline
thread_trampoline:
    sti
    mov rdi, rsi
    call rbx
    mov rdi, rax
    jmp thread_exit
