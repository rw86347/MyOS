; usermode.asm — the first entry into ring 3 (user_enter)
;
; void user_enter(uint64_t entry, uint64_t user_sp)
;
; Build the frame an interrupt from user mode would have left, and iretq
; "back" to it: CS = user code (ring 3), RFLAGS with only IF set, the
; program's stack. Every general register is zeroed first, so no kernel
; value leaks into the new program (Linux does the same at execve).
;
; FS and GS are left alone: their selectors are null in the kernel, and the
; program's FS base (thread-local storage) is an MSR (gdt_load_tls).
[bits 64]

GDT_USER_CODE equ 0x2B
GDT_USER_DATA equ 0x23

global user_enter
user_enter:
    push GDT_USER_DATA          ; ss
    push rsi                    ; rsp
    push 0x202                  ; rflags: IF on (bit 1 is always 1)
    push GDT_USER_CODE          ; cs
    push rdi                    ; rip
    xor eax, eax                ; (a 32-bit xor clears all 64 bits)
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq
