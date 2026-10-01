; =============================================================================
; hello.asm — the first user-mode program (ring 3)
; =============================================================================
; Built as an i386 ELF executable (nasm + i686-elf-ld; Linux puts i386
; programs at 0x08048000, and so does the linker). It talks to the kernel
; only through `int 0x80`, exactly like a Linux program:
;     EAX = syscall number, EBX, ECX, EDX = arguments, result in EAX.
;
; 1. write(1, msg, len)              -> prints the message
; 2. write(1, <kernel address>, 16)  -> must fail with -EFAULT (-14): the
;                                       kernel won't read its own memory for us
; 3. write(1, <unmapped address>, 16) -> must fail with -EFAULT too: the
;                                       kernel's copy faults, and the exception
;                                       table turns that into an error
; 4. exit(42) if both were refused, exit(1)/exit(2) if not
; =============================================================================

[bits 32]
global _start                   ; the linker makes this the ELF entry point

SYS_exit  equ 1
SYS_write equ 4
EFAULT    equ 14

section .text
_start:
    mov eax, SYS_write
    mov ebx, 1                  ; fd 1 = stdout
    mov ecx, msg
    mov edx, msg_len
    int 0x80

    mov eax, SYS_write          ; now try to print kernel memory
    mov ebx, 1
    mov ecx, 0xC0010000         ; the kernel's own code
    mov edx, 16
    int 0x80

    mov ebx, 1                  ; exit code 1 = the kernel leaked memory!
    cmp eax, -EFAULT
    jne .exit

    mov eax, SYS_write          ; a user address nothing is mapped at
    mov ebx, 1
    mov ecx, 0x10000000
    mov edx, 16
    int 0x80
    mov ebx, 2                  ; exit code 2 = no -EFAULT for unmapped memory
    cmp eax, -EFAULT
    jne .exit

    mov ebx, 42                 ; exit code 42 = "good, both were refused"
.exit:
    mov eax, SYS_exit
    int 0x80

section .rodata
msg:     db "Hello from user mode (ring 3), via int 0x80!", 10
msg_len  equ $ - msg
