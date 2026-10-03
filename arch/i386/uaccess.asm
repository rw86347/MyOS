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

; int __fault_in_user(uint32_t addr, uint32_t n, int write)
;   Touch one byte of every page in the range so that a later copy of the same
;   range cannot fault, and therefore cannot sleep. 0, or -EFAULT.
;
;   Why that matters: a fault on a page of an mmap'd file goes to the page
;   cache and waits for the disk, so another thread runs. A copy_to_user in
;   the middle of updating a ring buffer is therefore a place where two
;   processes can act on the same snapshot. Faulting the pages in first, while
;   nothing has been claimed yet, moves the only sleep to a harmless place.
;
;   The write probe is a read-modify-write with a no-op operand: `or ..., 0`
;   leaves the byte alone but still takes the write fault, which is what
;   breaks a copy-on-write page shared with a fork'd child. A plain read would
;   leave it shared and the real copy would fault after all.
global __fault_in_user
__fault_in_user:
    mov edx, [esp + 4]          ; addr
    mov ecx, [esp + 8]          ; n
    test ecx, ecx
    jz .ok                      ; nothing to touch
    add ecx, edx                ; one past the end; the caller range-checked
    cmp dword [esp + 12], 0     ; so this cannot wrap and edx stays < 0xC0000000
    jnz .wloop
.rloop:
.rprobe:
    mov al, [edx]               ; may fault: handled, or the fixup below
    and edx, 0xFFFFF000
    add edx, 0x1000             ; the next page
    cmp edx, ecx
    jb .rloop
.ok:
    xor eax, eax
    ret
.wloop:
.wprobe:
    or byte [edx], 0            ; same, but takes the *write* fault
    and edx, 0xFFFFF000
    add edx, 0x1000
    cmp edx, ecx
    jb .wloop
    jmp .ok
.fault:
    mov eax, -EFAULT
    ret
EX_ENTRY __fault_in_user.rprobe, __fault_in_user.fault
EX_ENTRY __fault_in_user.wprobe, __fault_in_user.fault

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
