; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; =============================================================================
; peek.asm — a misbehaving user program
; =============================================================================
; Tries to read kernel memory directly. Kernel pages don't have the "user"
; bit, so the CPU raises a page fault; the kernel must kill this program
; (like Linux's "Segmentation fault") and keep running.
; =============================================================================

[bits 32]
global _start

section .text
_start:
    mov eax, [0xC0010000]       ; read the kernel's first bytes -> page fault

    mov ebx, eax                ; never reached: exit(whatever we read)
    mov eax, 1
    int 0x80
