; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; entry.asm — first 64-bit kernel code
;
; The UEFI loader copied kernel.bin to physical 1 MiB and jumped here, in
; long mode, on the firmware's page tables (an identity map: virtual ==
; physical). But the kernel is LINKED to run at the top of memory
; (KERNEL_VBASE, -2 GiB; memlayout.h), so that programs get the bottom half
; from address 0, as on Linux.
;
; Until we jump up there, only RIP-relative code works: [rel x] means "x's
; distance from this instruction", and that distance is the same at both
; addresses. So, still at 1 MiB:
;
;   1. clear .bss — the boot page tables below live in it
;   2. build boot page tables: ONE page directory of 2 MiB pages covering
;      the first 1 GiB of RAM, hung in three places:
;        PML4[0]   identity          0 .. 1 GiB  (we are running here now)
;        PML4[256] direct map        DIRECTMAP_BASE + (0 .. 1 GiB)  (P2V)
;        PML4[511] kernel image      KERNEL_VBASE   + (0 .. 1 GiB)
;   3. load CR3, and jump to the high address of the next instruction
;   4. our GDT and stack, the stack-protector guard, and kernel_main
;
; Nothing the firmware owns may be touched after step 3 — not even its
; stack: it can lie above 1 GiB.
;
; vmm_init (paging.c) then replaces these tables with the real ones,
; which no longer have the identity map.

[bits 64]
default rel

KERNEL_VBASE equ 0xFFFFFFFF80000000
PG_P         equ 1 << 0                 ; present
PG_W         equ 1 << 1                 ; writable
PG_PS        equ 1 << 7                 ; (in a page directory) a 2 MiB page
CR4_PGE      equ 1 << 7

section .text.entry
global _start
extern kernel_main
extern __bss_start, __bss_end

_start:
    cli
    cld

    ; 1. Clear .bss.
    lea rdi, [__bss_start]
    lea rcx, [__bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb

    ; Our own stack, now: the firmware's may be anywhere in RAM (2.6 GiB on
    ; a 4 GiB machine), and once CR3 holds our tables only the first 1 GiB
    ; is mapped. The first push would fault, with no handler we can reach
    ; (the firmware's IDT is unmapped too): a triple fault, and the PC
    ; resets. This is boot_stack's low address; .reload_cs moves RSP up.
    lea rsp, [boot_stack_top]

    ; 2. The page directory: entry i maps physical i * 2 MiB.
    lea rdi, [boot_pd]
    mov eax, PG_P | PG_W | PG_PS
    mov ecx, 512
.fill:
    mov [rdi], rax
    add rax, 0x200000
    add rdi, 8
    dec ecx
    jnz .fill

    ; One PDPT entry covers 1 GiB. Entry 0 of boot_pdpt_low is 0..1 GiB of
    ; whichever PML4 slot points at it (identity and direct map share it);
    ; entry 510 of boot_pdpt_high is -2 GiB (bits 38:30 of KERNEL_VBASE).
    lea rax, [boot_pd]
    or rax, PG_P | PG_W
    mov [boot_pdpt_low], rax
    mov [boot_pdpt_high + 510 * 8], rax
    lea rax, [boot_pdpt_low]
    or rax, PG_P | PG_W
    mov [boot_pml4], rax
    mov [boot_pml4 + 256 * 8], rax
    lea rax, [boot_pdpt_high]
    or rax, PG_P | PG_W
    mov [boot_pml4 + 511 * 8], rax

    ; Global pages off: switching CR4.PGE off empties the whole TLB, so no
    ; stale firmware entry for a low address survives into user space.
    ; vmm_init turns it back on for our own kernel pages.
    mov rax, cr4
    and rax, ~CR4_PGE
    mov cr4, rax

    ; 3. Our tables, then up. (`mov rax, imm64` is an absolute address:
    ; the high one the linker gave .high.)
    lea rax, [boot_pml4]
    mov cr3, rax
    mov rax, .high
    jmp rax

.high:
    lgdt [gdt_ptr]                      ; (gdt.c installs the real one soon)
    push 0x08
    lea rax, [.reload_cs]
    push rax
    retfq

.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax                ; FS, GS null: their bases are MSRs (a
    mov fs, ax                  ; program's thread-local storage), and a null
    mov gs, ax                  ; selector is one iretq leaves alone

    lea rsp, [boot_stack_top]
    xor ebp, ebp

    ; The stack-protector guard (Makefile: KERNEL_SSP). Every protected C
    ; function copies __stack_chk_guard below its return address and checks
    ; it before returning, so it is chosen now, before the first C function
    ; runs, and never changes. Someone overrunning a kernel stack buffer
    ; must guess it. Its sources: the cycle counter (how long the firmware
    ; and the loader took, which varies from boot to boot), mixed with the
    ; CPU's random number generator (RDRAND, CPUID.1:ECX bit 30), which
    ; every 64-bit PC we aim at has. The low byte is 0, as on 64-bit Linux:
    ; a runaway string copy stops at a 0 byte, so it can't write the guard
    ; back unchanged.
    rdtsc
    shl rdx, 32
    or rax, rdx
    mov rsi, rax
    mov eax, 1
    cpuid                               ; (overwrites EBX, ECX, EDX)
    bt ecx, 30
    jnc .no_rdrand
    rdrand rax                          ; (if it fails: RAX = 0, no harm)
    xor rsi, rax
.no_rdrand:
    and rsi, ~0xFF
    mov [__stack_chk_guard], rsi

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

section .rodata
; The guard, above. In read-only data: written once, before vmm_init maps
; the kernel's read-only data read-only — then not even a bad kernel write
; can set it to a value an attacker knows.
global __stack_chk_guard
align 8
__stack_chk_guard:  dq 0

align 16
gdt:
    dq 0
    dw 0xFFFF, 0, 0x9A00, 0x00AF    ; 0x08: 64-bit code
    dw 0xFFFF, 0, 0x9200, 0x00AF    ; 0x10: 64-bit data
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dq gdt                          ; its high address

section .bss nobits alloc noexec write align=4096
boot_pml4:      resb 4096
boot_pdpt_low:  resb 4096
boot_pdpt_high: resb 4096
boot_pd:        resb 4096
alignb 16
boot_stack:
    resb 16384
global boot_stack_top                   ; (a crash's stack scan: where "main"'s stack ends)
boot_stack_top:
