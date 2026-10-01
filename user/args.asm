; =============================================================================
; args.asm — print the command line, to check the start-up stack
; =============================================================================
; At _start, the kernel has left this on the stack (see kernel/usermode.c):
;     [esp]       argc
;     [esp+4]     argv[0], argv[1], ..., NULL
; We print every argv[i] followed by a space, then a newline, and exit with
; argc as the exit code. Started with argv = {"args", "one", "two"} it
; prints "args one two " and exits with 3.
; =============================================================================

[bits 32]
global _start

SYS_exit  equ 1
SYS_write equ 4

section .text
_start:
    mov ebp, [esp]              ; ebp = argc
    lea esi, [esp + 4]          ; esi = &argv[0]

.next:
    mov ecx, [esi]              ; ecx = argv[i]
    test ecx, ecx
    jz .done                    ; NULL ends the list

    mov edx, 0                  ; edx = strlen(argv[i])
.len:
    cmp byte [ecx + edx], 0
    je .print
    inc edx
    jmp .len
.print:
    mov eax, SYS_write          ; write(1, argv[i], len)
    mov ebx, 1
    int 0x80
    mov eax, SYS_write          ; write(1, " ", 1)
    mov ebx, 1
    mov ecx, space
    mov edx, 1
    int 0x80
    add esi, 4
    jmp .next

.done:
    mov eax, SYS_write          ; write(1, "\n", 1)
    mov ebx, 1
    mov ecx, newline
    mov edx, 1
    int 0x80
    mov eax, SYS_exit           ; exit(argc)
    mov ebx, ebp
    int 0x80

section .rodata
space:   db " "
newline: db 10
