; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; =============================================================================
; uaccess.asm — touching user memory safely and quickly (x86_64)
; =============================================================================
; The twin of arch/i386/uaccess.asm; read that file's header first. Every
; instruction that touches user memory is in the EXCEPTION TABLE (section
; __ex_table) with a "fixup" address. On a kernel page fault, interrupts.c
; looks the faulting RIP up there and, if found, continues at the fixup,
; which returns -EFAULT instead of crashing the kernel. The C wrappers in
; uaccess.h have already checked the range lies in user space.
;
; The entries are 64-bit addresses (dq): the kernel is at -2 GiB, which a
; 32-bit `dd` cannot hold. The label must be ON the instruction that can
; fault (the `rep movs`, the load), not on one before it.
;
; System V calling convention: arguments in rdi, rsi, rdx, rcx; result in
; rax; rdi, rsi, rcx, rdx, r8-r11 may be clobbered (no saving needed).
; =============================================================================

[bits 64]
default rel
EFAULT equ 14

; SMAP: open the window to user memory (stac) for one access, and shut it
; (clac) on every way out, the fault's included. Only when the CPU has SMAP
; (smap_on, paging.c): stac/clac don't exist before it. A load and a
; branch the CPU always predicts.
extern smap_on
%macro USER_BEGIN 0
    cmp byte [rel smap_on], 0
    je %%no
    stac
%%no:
%endmacro
%macro USER_END 0
    cmp byte [rel smap_on], 0
    je %%no
    clac
%%no:
%endmacro

%macro EX_ENTRY 2               ; instruction that may fault, where to go then
  section __ex_table progbits alloc noexec nowrite align=8
    dq %1, %2
  section .text
%endmacro

section .text

; int __copy_user(void *dst, const void *src, uint64_t n)
;   Copy n bytes; either side may be user memory. 0, or -EFAULT.
global __copy_user
__copy_user:
    USER_BEGIN
    mov rcx, rdx                ; rdi = dst, rsi = src already: what movs wants
    shr rcx, 3
.qwords:
    rep movsq                   ; bulk, 8 bytes per step
    mov ecx, edx
    and ecx, 7
.bytes:
    rep movsb                   ; the last 0-7 bytes
    USER_END
    xor eax, eax
    ret
.fault:
    USER_END
    mov eax, -EFAULT
    ret
EX_ENTRY __copy_user.qwords, __copy_user.fault
EX_ENTRY __copy_user.bytes, __copy_user.fault

; int __get_user32(uint32_t *dst, uint64_t user_addr)
;   Read one 32-bit word from user memory. 0, or -EFAULT.
global __get_user32
__get_user32:
    USER_BEGIN
.load:
    mov eax, [rsi]
    USER_END
    mov [rdi], eax
    xor eax, eax
    ret
.fault:
    USER_END
    mov eax, -EFAULT
    ret
EX_ENTRY __get_user32.load, __get_user32.fault

; int __get_user64(uint64_t *dst, uint64_t user_addr)
;   Read one 64-bit word (a pointer in an argv array, an iovec). 0, or -EFAULT.
global __get_user64
__get_user64:
    USER_BEGIN
.load:
    mov rax, [rsi]
    USER_END
    mov [rdi], rax
    xor eax, eax
    ret
.fault:
    USER_END
    mov eax, -EFAULT
    ret
EX_ENTRY __get_user64.load, __get_user64.fault

; int __fault_in_user(uint64_t addr, uint64_t n, int write)
;   Touch one byte of every page in the range so that a later copy of it
;   cannot fault, and so cannot sleep. 0, or -EFAULT. (Why, and why the
;   write probe is `or [x], 0`: arch/i386/uaccess.asm.)
global __fault_in_user
__fault_in_user:
    USER_BEGIN
    test rsi, rsi
    jz .ok                      ; nothing to touch
    add rsi, rdi                ; one past the end; the caller range-checked,
    test edx, edx               ; so this cannot wrap
    jnz .wloop
.rloop:
.rprobe:
    mov al, [rdi]               ; may fault: handled, or the fixup below
    and rdi, ~0xFFF
    add rdi, 0x1000             ; the next page
    cmp rdi, rsi
    jb .rloop
.ok:
    USER_END
    xor eax, eax
    ret
.wloop:
.wprobe:
    or byte [rdi], 0            ; same, but takes the *write* fault
    and rdi, ~0xFFF
    add rdi, 0x1000
    cmp rdi, rsi
    jb .wloop
    jmp .ok
.fault:
    USER_END
    mov eax, -EFAULT
    ret
EX_ENTRY __fault_in_user.rprobe, __fault_in_user.fault
EX_ENTRY __fault_in_user.wprobe, __fault_in_user.fault

; long __strncpy_user(char *dst, const char *user_src, long max)
;   Copy a NUL-terminated string of at most max bytes (including the NUL).
;   Returns its length without the NUL, max if no NUL was found, or -EFAULT.
global __strncpy_user
__strncpy_user:
    USER_BEGIN
    xor eax, eax                ; rax = length so far
.next:
    cmp rax, rdx
    jae .done                   ; max reached without a NUL
.load:
    mov cl, [rsi + rax]
    mov [rdi + rax], cl
    test cl, cl
    jz .done
    inc rax
    jmp .next
.done:
    USER_END
    ret
.fault:
    USER_END
    mov rax, -EFAULT
    ret
EX_ENTRY __strncpy_user.load, __strncpy_user.fault
