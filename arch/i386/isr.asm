; =============================================================================
; isr.asm — interrupt entry stubs ("Interrupt Service Routines")
; =============================================================================
; When an interrupt or CPU exception (fault) happens, the CPU:
;   1. looks up the vector number (0-255) in the IDT to find a handler address,
;   2. pushes EFLAGS, CS and EIP (the place to return to) on the stack —
;      and for a few exceptions, an extra "error code",
;   3. clears the interrupt flag (we use "interrupt gates"), and jumps.
;
; Vectors we use:
;   0-31   CPU exceptions (divide error, page fault, general protection, ...)
;   32-47  hardware IRQs 0-15, after the PIC is remapped (see pic.c)
;
; Each stub pushes a dummy error code if the CPU didn't push one (so the stack
; always looks the same), pushes its vector number, and jumps to common_stub.
; common_stub saves all registers and calls interrupt_dispatch(struct regs *)
; in interrupts.c. The layout of what we push must match struct regs exactly.
; =============================================================================

[bits 32]
extern interrupt_dispatch

; Exceptions where the CPU pushes an error code itself.
%macro STUB_ERR 1
vec%1:
    push dword %1               ; vector number
    jmp common_stub
%endmacro

; Everything else: push a fake error code 0 first.
%macro STUB_NOERR 1
vec%1:
    push dword 0                ; dummy error code
    push dword %1               ; vector number
    jmp common_stub
%endmacro

section .text

; Generate the 48 stubs. Exceptions 8, 10-14, 17, 21, 29, 30 have error codes.
%assign i 0
%rep 48
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    STUB_ERR i
  %else
    STUB_NOERR i
  %endif
  %assign i i+1
%endrep

common_stub:
    pushad                      ; push EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10                ; make sure we use kernel data segments
    mov ds, ax                  ; (matters once interrupts can come from
    mov es, ax                  ;  user mode, step 5)
    mov fs, ax
    mov gs, ax
    cld                         ; C code expects the direction flag clear

    push esp                    ; argument: pointer to the saved registers
    call interrupt_dispatch
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8                  ; drop vector number + error code
    iretd                       ; return to the interrupted code (pops EIP, CS, EFLAGS)

; Table of the 48 stub addresses, so C can fill in the IDT with a loop.
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dd vec %+ i
  %assign i i+1
%endrep
