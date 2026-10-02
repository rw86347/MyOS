; =============================================================================
; crt0.asm — where a C test program starts ("C run-time zero")
; =============================================================================
; The kernel jumps to _start with ESP pointing at argc (see
; kernel/usermode.c for the whole start-up stack). We hand that pointer to
; __libc_start (mini.c), which finds argv and the auxiliary vector, calls
; main(), and exits with its return value.
; =============================================================================

[bits 32]
global _start, __restore, __restore_rt
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

; Signal "restorers": a handler returns here, and these ask the kernel to
; put the interrupted program back (sigreturn). Same code as musl's.
__restore:                      ; for handler(sig)
    pop eax                     ; drop the signal number
    mov eax, 119                ; sigreturn
    int 0x80
__restore_rt:                   ; for handler(sig, info, ucontext)
    mov eax, 173                ; rt_sigreturn
    int 0x80
