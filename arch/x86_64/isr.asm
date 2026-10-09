; isr.asm — x86_64 interrupt stubs (twins of arch/i386/isr.asm)
[bits 64]

extern interrupt_dispatch

%macro ISR_PUSH_ERR 1
global isr %+ %1
isr %+ %1:
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_PUSH_ZERO 1
global isr %+ %1
isr %+ %1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

section .text

; 0-31 CPU exceptions, 32-47 the 8259's IRQs 0-15, 48-63 MSI IRQs 16-31.
%assign v 0
%rep 64
  %if v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30
    ISR_PUSH_ERR v
  %else
    ISR_PUSH_ZERO v
  %endif
  %assign v v+1
%endrep

; Vector 0xFF: the local APIC's "spurious" interrupt (one it had to take
; back). It wants no EOI and there is nothing to do: return at once.
global isr_spurious
isr_spurious:
    iretq

isr_common:
    ; The direction flag first. An interrupt or fault keeps the interrupted
    ; code's flags, and a program may have DF set at that moment (musl's
    ; memmove: std; rep movs; cld). With DF set, every kernel memcpy and
    ; memset runs backwards, over whatever lies below its target — that
    ; once wrote the console's grid back over the whole kernel image and
    ; the IDT (a triple fault). C code expects it clear; iretq gives the
    ; program its own flag back. (syscall clears it through SFMASK.)
    cld
    push rax
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

    mov rdi, rsp
    call interrupt_dispatch

global isr_return
isr_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 64
    dq isr %+ v
  %assign v v+1
%endrep
