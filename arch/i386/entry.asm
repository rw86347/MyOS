; =============================================================================
; entry.asm — first kernel code to run (32-bit protected mode)
; =============================================================================
; The MBR jumps to KERNEL_LOAD (0x10000). The linker script (linker.ld) puts
; the .text.entry section first, so _start sits exactly at that address.
;
; On arrival: protected mode, flat segments, interrupts off, ESP = 0x90000.

[bits 32]

section .text.entry
global _start
extern kernel_main
extern __bss_start, __bss_end     ; defined in linker.ld

_start:
    ; Zero the .bss section. C expects uninitialized globals to be 0, but the
    ; MBR only copied the bytes that exist in kernel.bin from disk — .bss is
    ; not stored in the file, so that memory holds whatever was there before.
    cld                             ; make rep stosb count upward
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi                    ; ecx = number of bytes to clear
    xor eax, eax
    rep stosb                       ; store AL (0) at [EDI], ECX times

    call kernel_main                ; into C!

    ; kernel_main should never return, but if it does, stop the CPU forever.
.hang:
    cli
    hlt
    jmp .hang
