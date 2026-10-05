; entry.asm — first 64-bit kernel code
;
; The UEFI loader left us in long mode with an identity map and jumped
; here at physical 1 MiB. We load our own GDT (firmware's CS is fine for
; a jump, but C and later interrupt return need a known layout), clear
; .bss, switch to our stack, and call kernel_main.

[bits 64]

section .text.entry
global _start
extern kernel_main
extern __bss_start, __bss_end

_start:
    cli
    lea rax, [rel gdt]
    mov [rel gdt_ptr + 2], rax
    lgdt [rel gdt_ptr]

    push 0x08
    lea rax, [rel reload_cs]
    push rax
    retfq

reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    cld
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb

    lea rsp, [rel boot_stack_top]
    and rsp, ~15
    xor rbp, rbp
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

align 16
gdt:
    dq 0
    dw 0xFFFF, 0, 0x9A00, 0x00AF    ; 0x08: 64-bit code
    dw 0xFFFF, 0, 0x9200, 0x00AF    ; 0x10: 64-bit data
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dq 0                            ; filled at run time (relocatable)

section .bss
align 16
boot_stack:
    resb 16384
boot_stack_top:
