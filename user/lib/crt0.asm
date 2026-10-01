; =============================================================================
; crt0.asm — where a C test program starts ("C run-time zero")
; =============================================================================
; The kernel jumps to _start with ESP pointing at argc (see
; kernel/usermode.c for the whole start-up stack). We hand that pointer to
; __libc_start (mini.c), which finds argv and the auxiliary vector, calls
; main(), and exits with its return value.
; =============================================================================

[bits 32]
global _start
extern __libc_start

section .text
_start:
    xor ebp, ebp                ; mark the end of the call-frame chain
    mov eax, esp                ; eax = &argc
    and esp, -16                ; the i386 ABI wants ESP 16-byte aligned...
    sub esp, 12                 ; ...at the call, after pushing the argument
    push eax
    call __libc_start           ; never returns
    hlt                         ; (a privileged instruction: faults if reached)
