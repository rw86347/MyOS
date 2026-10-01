; =============================================================================
; fpu.asm — do my FPU and SSE registers survive being switched out?
; =============================================================================
; Puts a known number in the x87 register st0 and a known pattern in the SSE
; register xmm0, then spins for a while. The timer switches us out many
; times; meanwhile the kernel test runs a thread that wipes both registers.
; If the kernel saves and restores them correctly, we find our values
; unchanged afterwards.
;
; exit(0) = both survived, exit(1) = st0 was lost, exit(2) = xmm0 was lost
; (killed with 139 = SSE isn't enabled: movups faulted)
; =============================================================================

[bits 32]
global _start

SYS_exit equ 1
SPINS    equ 10000000

section .text
_start:
    fld qword [number]          ; st0 = 1234.5
    movups xmm0, [pattern]      ; xmm0 = 16 known bytes

    mov ecx, SPINS              ; spin: plenty of timer ticks go by
.spin:
    dec ecx
    jnz .spin

    ; --- x87: compare st0 with the number again -------------------------
    fld qword [number]          ; st0 = 1234.5, st1 = what we kept
    fcomip st0, st1             ; compare, set CPU flags, pop
    mov ebx, 1
    jp .exit                    ; "unordered": st1 was empty or NaN
    jne .exit                   ; different number

    ; --- SSE: store xmm0 and compare the 16 bytes ------------------------
    movups [result], xmm0
    mov esi, result
    mov edi, pattern
    mov ecx, 4
    repe cmpsd                  ; compare 4 dwords
    mov ebx, 2
    jne .exit

    xor ebx, ebx                ; all good: exit(0)
.exit:
    mov eax, SYS_exit
    int 0x80

section .rodata
number:  dq 1234.5
pattern: dd 0x11111111, 0x22222222, 0x33333333, 0x44444444

section .bss
result:  resb 16                ; (.bss: tests that the loader zero-fills too)
