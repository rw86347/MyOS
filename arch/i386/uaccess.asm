; =============================================================================
; uaccess.asm — touching user memory safely and quickly
; =============================================================================
; A system call gets pointers from a user program, and they may be garbage:
; unmapped, or pointing at a read-only page. The slow way is to walk the page
; tables first and check every page. The fast way (Linux's) is to just do the
; copy — and if it faults, recover:
;
;   Every instruction here that touches user memory is listed in the
;   EXCEPTION TABLE (section __ex_table) together with a "fixup" address.
;   When the kernel page-faults, the handler (interrupts.c) looks up the
;   faulting EIP in that table; if it's there, it jumps to the fixup, which
;   returns -EFAULT, instead of crashing the kernel.
;
; The common case — a good pointer — costs nothing extra. The C wrappers in
; uaccess.h first check that the range is below 0xC0000000 (user space), so
; a program can't make us copy kernel memory.
; =============================================================================

[bits 32]
EFAULT equ 14

%macro EX_ENTRY 2               ; instruction that may fault, where to go then
  section __ex_table progbits alloc noexec nowrite align=4
    dd %1, %2
  section .text
%endmacro

section .text

; int __copy_user(void *dst, const void *src, size_t n)
;   Copy n bytes; either side may be user memory. 0, or -EFAULT.
global __copy_user
__copy_user:
    push esi
    push edi
    mov edi, [esp + 12]
    mov esi, [esp + 16]
    mov ecx, [esp + 20]
    mov edx, ecx
    shr ecx, 2
.words:
    rep movsd                   ; bulk, 4 bytes per step
    mov ecx, edx
    and ecx, 3
.bytes:
    rep movsb                   ; the last 0-3 bytes
    xor eax, eax
.out:
    pop edi
    pop esi
    ret
.fault:
    mov eax, -EFAULT
    jmp .out
EX_ENTRY __copy_user.words, __copy_user.fault
EX_ENTRY __copy_user.bytes, __copy_user.fault

; int __get_user32(uint32_t *dst, uint32_t user_addr)
;   Read one 32-bit word from user memory. 0, or -EFAULT.
global __get_user32
__get_user32:
    mov edx, [esp + 8]
.load:
    mov eax, [edx]
    mov edx, [esp + 4]
    mov [edx], eax
    xor eax, eax
    ret
.fault:
    mov eax, -EFAULT
    ret
EX_ENTRY __get_user32.load, __get_user32.fault

; long __strncpy_user(char *dst, const char *user_src, long max)
;   Copy a NUL-terminated string of at most max bytes (including the NUL).
;   Returns its length without the NUL, max if no NUL was found, or -EFAULT.
global __strncpy_user
__strncpy_user:
    push esi
    push edi
    mov edi, [esp + 12]
    mov esi, [esp + 16]
    mov ecx, [esp + 20]
    xor eax, eax                ; eax = length so far
.next:
    cmp eax, ecx
    jae .done                   ; max reached without a NUL
.load:
    mov dl, [esi + eax]
    mov [edi + eax], dl
    test dl, dl
    jz .done
    inc eax
    jmp .next
.done:
    pop edi
    pop esi
    ret
.fault:
    mov eax, -EFAULT
    jmp .done
EX_ENTRY __strncpy_user.load, __strncpy_user.fault
