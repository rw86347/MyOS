; =============================================================================
; true.asm — the smallest possible program: exit(0) at once
; =============================================================================
; Used by the benchmarks to time starting and ending a program (load the
; ELF file, build its stack, create a thread, run, exit, clean up).
; =============================================================================

[bits 32]
global _start

section .text
_start:
    mov eax, 1                  ; exit
    xor ebx, ebx                ; code 0
    int 0x80
