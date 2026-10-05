; syscall.asm — Linux x86_64 SYSCALL entry (MSR LSTAR -> here)
[bits 64]

extern syscall_entry_c
extern kernel_tss

GDT_USER_CODE equ 0x1B
GDT_USER_DATA equ 0x23

global syscall_entry_asm
syscall_entry_asm:
    swapgs
    mov r10, rsp
    mov rsp, qword [rel kernel_tss + 4]

    push qword 0
    push qword 128
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

    push GDT_USER_DATA
    push r10
    push r11
    push GDT_USER_CODE
    push rcx

    mov rdi, rsp
    call syscall_entry_c

    pop rcx
    add rsp, 32
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
    pop rsp
    swapgs
    o64 sysret                  ; (NASM's spelling of sysretq: back to 64-bit user mode)
