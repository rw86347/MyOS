[bits 64]
EFAULT equ 14

%macro EX_ENTRY 2
  section __ex_table progbits alloc noexec nowrite align=4
    dd %1, %2
  section .text
%endmacro

section .text

global __copy_user
__copy_user:
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rdx
    mov rdx, rcx
    test rdx, rdx
    jz .out
.wq:
    mov rcx, rdx
    shr rcx, 3
    jz .wb
    rep movsq
.wb:
    mov rcx, rdx
    and rcx, 7
    jz .out
    rep movsb
    xor eax, eax
.out:
    ret
.fault:
    mov eax, -EFAULT
    ret
EX_ENTRY __copy_user.wq, __copy_user.fault
EX_ENTRY __copy_user.wb, __copy_user.fault

global __get_user32
__get_user32:
    mov eax, [rsi]
    mov [rdi], eax
    xor eax, eax
    ret
.gf:
    mov eax, -EFAULT
    ret
EX_ENTRY __get_user32, __get_user32.gf

global __fault_in_user
__fault_in_user:
    test rsi, rsi
    jz .ok
    mov rcx, rsi
    add rcx, rdi
    cmp qword [rsp + 8], 0
    jnz .wloop
.rloop:
    mov al, [rdi]
    and rdi, ~0xFFF
    add rdi, 4096
    cmp rdi, rcx
    jb .rloop
.ok:
    xor eax, eax
    ret
.wloop:
    or byte [rdi], 0
    and rdi, ~0xFFF
    add rdi, 4096
    cmp rdi, rcx
    jb .wloop
    jmp .ok
.wf:
    mov eax, -EFAULT
    ret
EX_ENTRY __fault_in_user.rloop, __fault_in_user.wf
EX_ENTRY __fault_in_user.wloop, __fault_in_user.wf

global __strncpy_user
__strncpy_user:
    xor rax, rax
.next:
    cmp rax, rdx
    jae .done
    mov cl, [rsi + rax]
    mov [rdi + rax], cl
    test cl, cl
    jz .done
    inc rax
    jmp .next
.done:
    ret
.sf:
    mov rax, -EFAULT
    ret
EX_ENTRY __strncpy_user.next, __strncpy_user.sf
