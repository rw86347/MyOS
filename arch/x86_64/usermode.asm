; usermode.asm — iretq into ring 3 (user_enter)
[bits 64]

GDT_USER_CODE equ 0x1B
GDT_USER_DATA equ 0x23

global user_enter
user_enter:
    mov rax, rdi
    mov rdx, rsi
    mov ax, GDT_USER_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push GDT_USER_DATA
    push rdx
    pushfq
    pop rcx
    or rcx, 0x200
    push rcx
    push GDT_USER_CODE
    push rax
    iretq
