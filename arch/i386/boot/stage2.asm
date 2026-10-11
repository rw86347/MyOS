; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Rodger Wilson
; =============================================================================
; stage2.asm — MyOS stage 2 bootloader
; =============================================================================
; The MBR loaded us at 0x7E00 and jumped here (real mode, DL = boot drive).
; Our jobs:
;   1. ask the BIOS for the memory map (E820) — only possible in real mode
;   2. enable the A20 line, so memory above 1 MiB is reachable
;   3. show the boot picture (VGA mode 13h, 320x200) and load the kernel
;      to 1 MiB and the initramfs right after it, with a progress bar
;   4. leave a note for the kernel at BOOTINFO (bootinfo.h)
;   5. back to text mode, then 32-bit protected mode and jump to the kernel
;
; The tricky part is 3. In real mode the BIOS disk call can only write into
; the first 1 MiB of memory. So we load in chunks: read 32 KiB into a
; "bounce buffer" below 1 MiB, then ask the BIOS to copy it above 1 MiB with
; int 0x15, AH=0x87 ("move extended memory block", in every PC BIOS since the
; 286). Repeat until done — any size, and it only uses the BIOS, so it works
; when booting from a USB stick or CD on real hardware too.
;
; Passed in by the Makefile with -D:
;   SPLASH_LBA, SPLASH_SECTORS       the 320x200 picture (tools/mksplash.py)
;   KERNEL_LBA, KERNEL_SECTORS       where kernel.bin is on the disk
;   INITRD_LBA, INITRD_SECTORS       where the initramfs is on the disk
;   INITRD_BYTES                     its exact size
;   INITRD_LOAD                      where to put it in RAM (first page after
;                                    the kernel's .bss)
; =============================================================================

[bits 16]
%include "layout.inc"
[org STAGE2_LOAD]

stage2:
    xor ax, ax                  ; the MBR set these already; be sure
    mov ds, ax
    mov es, ax
    mov [boot_drive], dl

    ; --- 1. Memory map --------------------------------------------------------
    call read_e820

    ; --- 2. Enable the A20 line -------------------------------------------------
    ; Historical quirk: the 8086 could only address 1 MiB, and addresses
    ; "wrapped around". To stay compatible, PCs keep address line 20 forced to
    ; 0 until software turns it on. "Fast A20" via port 0x92 works on nearly
    ; all machines from the Pentium era onward.
    in al, 0x92
    or al, 2                    ; bit 1 = A20 enable
    and al, 0xFE                ; never write bit 0 (it resets the machine!)
    out 0x92, al

    ; --- 3. Picture, then the kernel and the initramfs --------------------------
    ; Mode 13h while still in real mode: the BIOS call goes away once we
    ; enter protected mode. The picture is one byte per pixel at 0xA0000.
    call graphics_on
    call load_logo
    mov dword [prog_done], 0
    mov dword [prog_total], KERNEL_SECTORS + INITRD_SECTORS
    call draw_bar

    mov eax, KERNEL_LBA
    mov ecx, KERNEL_SECTORS
    mov edi, KERNEL_LOAD
    call load

    mov eax, INITRD_LBA
    mov ecx, INITRD_SECTORS
    mov edi, INITRD_LOAD
    call load

    call hold                       ; so the picture can be seen; QEMU loads fast
    call graphics_off               ; kernel console is the VGA text screen

    ; --- 4. Tell the kernel where the initramfs is ------------------------------
    mov dword [BOOTINFO + 0], INITRD_LOAD
    mov dword [BOOTINFO + 4], INITRD_BYTES
    call copy_font

    ; --- 5. Switch to protected mode ------------------------------------------
    cli                         ; BIOS interrupt handlers are 16-bit: keep them off
    lgdt [gdt_descriptor]       ; tell the CPU where our GDT is
    mov eax, cr0
    or eax, 1                   ; CR0 bit 0 = PE (Protection Enable)
    mov cr0, eax
    ; We are now in protected mode, but CS still holds a real-mode value.
    ; A far jump reloads CS with our 32-bit code segment selector (0x08).
    jmp CODE_SEG:protected_mode

; -----------------------------------------------------------------------------
; load: copy ECX sectors, starting at disk sector EAX, to physical address EDI
; (anywhere in memory, also above 1 MiB). Advances the progress bar per chunk.
; -----------------------------------------------------------------------------
load:
.next:
    test ecx, ecx
    jz .done
    mov ebx, ecx                ; ebx = sectors in this chunk
    cmp ebx, CHUNK_SECTORS
    jbe .read
    mov ebx, CHUNK_SECTORS
.read:
    call read_bounce            ; disk -> bounce buffer

    ; bounce buffer -> EDI, BIOS int 0x15 AH=0x87. The BIOS wants a little
    ; GDT (move_gdt) whose entries 2 and 3 describe the source and the
    ; destination; we patch the destination's base address = EDI.
    mov edx, edi
    mov [move_dst + 2], dx      ; base bits 0-15
    shr edx, 16
    mov [move_dst + 4], dl      ; base bits 16-23
    mov [move_dst + 7], dh      ; base bits 24-31
    push ax                     ; (EAX is the disk sector we're at: keep it)
    xor ax, ax                  ; ES must be 0: the GDT lives in the first 64 KiB
    mov es, ax
    pop ax
    pushad
    mov cx, bx
    shl cx, 8                   ; CX = 16-bit words to copy (sectors * 256)
    mov si, move_gdt            ; ES:SI = the table
    mov ah, 0x87
    int 0x15
    popad
    jc move_error
    add [prog_done], ebx
    call draw_bar

    ; next chunk
    add eax, ebx                ; next disk sector
    sub ecx, ebx                ; sectors left
    shl ebx, 9                  ; sectors -> bytes
    add edi, ebx                ; next destination
    jmp .next
.done:
    ret

; read_bounce: BIOS read of EBX sectors at LBA EAX into the bounce buffer.
read_bounce:
    mov [dap_count], bx
    mov [dap_lba], eax
    mov word [dap_off], 0
    mov word [dap_seg], BOUNCE >> 4
    pushad                      ; BIOS calls may change registers
    mov si, dap                 ; (pushad/popad don't touch the flags, so
    mov ah, 0x42                ;  the carry flag survives popad)
    mov dl, [boot_drive]
    int 0x13
    popad
    jc disk_error
    ret

; graphics_on: 320x200, 256 colours. Palette entries 0..15 are a grey ramp
; matching the bytes tools/mksplash.py writes (0 black, 15 white).
graphics_on:
    mov ax, 0x0013
    int 0x10
    mov dx, 0x3C8
    xor al, al
    out dx, al
    inc dx                      ; 0x3C9, palette data
    xor bx, bx
.pel:
    mov al, bl
    mov ah, 63
    mul ah                      ; ax = level * 63
    mov cl, 15
    div cl                      ; al = level * 63 / 15  (VGA DAC is 6-bit)
    out dx, al
    out dx, al
    out dx, al
    inc bx
    cmp bx, 16
    jb .pel
    ret

graphics_off:
    mov ax, 0x0003              ; 80x25 text, what the kernel console expects
    int 0x10
    ret

; copy_font: the video BIOS's 8x16 character shapes, for the kernel's pixel
; console (drivers/video/fbcon.c) — once the screen is in a graphics mode the
; card no longer draws text, so the kernel draws the letters itself. int 0x10
; AX=0x1130 BH=6 returns ES:BP pointing at the font: 256 characters, 16
; bytes each, one byte per row, bit 7 the leftmost pixel. We copy it to FONT
; and leave its address in the bootinfo (font_phys).
copy_font:
    push ds
    mov ax, 0x1130
    mov bh, 6
    int 0x10
    mov ax, es
    mov ds, ax
    mov si, bp                  ; DS:SI = the BIOS's font
    xor ax, ax
    mov es, ax
    mov di, FONT                ; ES:DI = FONT
    mov cx, 4096 / 2
    cld
    rep movsw
    pop ds
    mov dword [BOOTINFO + BOOTINFO_FONT], FONT
    xor eax, eax                    ; no GOP on a BIOS boot
    mov dword [BOOTINFO + BOOTINFO_FONT + 4], eax
    mov dword [BOOTINFO + BOOTINFO_FONT + 8], eax
    mov dword [BOOTINFO + BOOTINFO_FONT + 12], eax
    mov dword [BOOTINFO + BOOTINFO_FONT + 16], eax
    mov dword [BOOTINFO + BOOTINFO_FONT + 20], eax
    mov dword [BOOTINFO + BOOTINFO_FONT + 24], eax
    mov dword [BOOTINFO + BOOTINFO_FONT + 28], eax
    mov dword [BOOTINFO + BOOTINFO_FONT + 32], eax   ; fb_phys_hi
    ret

; hold: ~2 seconds, so the picture is visible. QEMU finishes the disk copy
; in a flash. The BIOS tick counter (int 0x1A) runs at 18.2 Hz. Disk and
; video calls can come back with interrupts off, which freezes that counter,
; so interrupts are turned on first. A spin cap keeps a broken clock from
; hanging the boot.
hold:
    sti
    call segs                   ; BIOS calls may return with DS changed
    mov ah, 0
    int 0x1A
    call segs
    mov [hold_tick], dx
    mov dword [hold_spins], 0
.wait:
    mov ah, 0
    int 0x1A
    call segs
    sub dx, [hold_tick]
    cmp dx, 36                  ; 36/18.2 ≈ 2 seconds
    jae .done
    inc dword [hold_spins]
    cmp dword [hold_spins], 2000000
    jb .wait
.done:
    ret

; segs: DS = ES = 0, which is where our data and the BIOS packet live.
segs:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    ret

; load_logo: the picture is below 1 MiB, so the BIOS can read it into the
; bounce buffer and we copy it onto the screen. 0xA0000 is mode 13h's buffer.
load_logo:
    mov eax, SPLASH_LBA
    mov ecx, SPLASH_SECTORS
    xor di, di
.next:
    test ecx, ecx
    jz .done
    mov ebx, ecx
    cmp ebx, CHUNK_SECTORS
    jbe .read
    mov ebx, CHUNK_SECTORS
.read:
    call read_bounce
    push eax
    push ecx
    push ds
    mov ax, BOUNCE >> 4
    mov ds, ax
    mov ax, 0xA000
    mov es, ax
    xor si, si
    mov cx, bx
    shl cx, 8                   ; words = sectors * 256
    cld
    rep movsw                   ; di advances across the screen
    pop ds
    xor ax, ax
    mov es, ax
    pop ecx
    pop eax
    add eax, ebx
    sub ecx, ebx
    jmp .next
.done:
    ret

; draw_bar: a 300-pixel line near the bottom. Grey track, white fill for
; prog_done/prog_total. Preserves every register load() is using.
draw_bar:
    pushad
    push es
    mov ax, 0xA000
    mov es, ax
    mov eax, [prog_done]
    mov ebx, 300
    mul ebx
    cmp dword [prog_total], 0
    je .empty
    xor edx, edx
    div dword [prog_total]
    cmp eax, 300
    jbe .width
    mov eax, 300
.width:
    mov ecx, eax
    jmp .paint
.empty:
    xor ecx, ecx
.paint:
    mov di, 198 * 320 + 10
    push cx
    mov cx, 300
    mov al, 8
    cld
    rep stosb
    pop cx
    mov di, 198 * 320 + 10
    mov al, 15
    rep stosb
    pop es
    popad
    ret

; -----------------------------------------------------------------------------
; read_e820: ask the BIOS for the memory map (int 0x15, EAX=0xE820)
; -----------------------------------------------------------------------------
; The only reliable way to learn where RAM is (and which ranges belong to the
; BIOS, ACPI, PCI devices...) — and it only works in real mode. Each call
; returns one 24-byte entry { u64 base, u64 length, u32 type, u32 acpi } at
; ES:DI; EBX is a "continuation" value: 0 means that was the last entry.
; Result (bootinfo.h): [BOOTINFO+8] = number of entries, entries from +12.
read_e820:
    xor ebx, ebx
    xor bp, bp                  ; bp = entry count
    mov di, BOOTINFO + 12
.next:
    mov eax, 0xE820
    mov edx, 0x534D4150         ; 'SMAP' — magic the BIOS checks
    mov ecx, 24
    mov dword [di + 20], 1      ; preset "ACPI: entry valid" for old BIOSes
    int 0x15
    jc .done                    ; carry: error or no more entries
    cmp eax, 0x534D4150         ; BIOS answers with 'SMAP' too
    jne .done
    inc bp
    add di, 24
    cmp bp, E820_MAX
    jae .done
    test ebx, ebx
    jnz .next
.done:
    movzx ebp, bp
    mov [BOOTINFO + 8], ebp
    ret

disk_error:
    mov si, msg_disk_error
    jmp fail
move_error:
    mov si, msg_move_error
fail:
    call graphics_off           ; errors are printed on the text screen
    call print16
.hang:
    hlt
    jmp .hang

; print16: print the zero-terminated string at DS:SI using the BIOS (real mode only)
print16:
    mov ah, 0x0E                ; BIOS teletype output
.next:
    lodsb                       ; al = [si], si++
    test al, al
    jz .done
    int 0x10
    jmp .next
.done:
    ret

; -----------------------------------------------------------------------------
; 32-bit protected mode code
; -----------------------------------------------------------------------------
[bits 32]
protected_mode:
    ; Load every data segment register with our data segment selector (0x10).
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000            ; a temporary stack; the kernel sets its own

    ; Jump to the kernel's entry point (_start in arch/i386/entry.asm).
    jmp KERNEL_LOAD

; -----------------------------------------------------------------------------
; Data
; -----------------------------------------------------------------------------
; GDT for protected mode — the "flat model": one code and one data segment,
; both starting at 0 and covering all 4 GiB (see the kernel's gdt.c for more).
; The kernel installs its own GDT soon after.
align 8
gdt_start:
    dq 0                        ; entry 0: the mandatory "null descriptor"
gdt_code:                       ; entry 1 (selector 0x08): code, ring 0
    dw 0xFFFF                   ; limit bits 0-15
    dw 0x0000                   ; base bits 0-15
    db 0x00                     ; base bits 16-23
    db 10011010b                ; access: present, ring 0, code, executable, readable
    db 11001111b                ; flags: 4 KiB granularity, 32-bit; limit bits 16-19
    db 0x00                     ; base bits 24-31
gdt_data:                       ; entry 2 (selector 0x10): data, ring 0
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                ; access: present, ring 0, data, writable
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:                 ; the 6-byte value that lgdt loads
    dw gdt_end - gdt_start - 1  ; size of the table minus one
    dd gdt_start                ; linear address of the table

CODE_SEG equ gdt_code - gdt_start   ; = 0x08
DATA_SEG equ gdt_data - gdt_start   ; = 0x10

; The table int 0x15 AH=0x87 wants: 6 descriptors. The BIOS fills in 0, 1,
; 4 and 5 itself; we provide 2 (copy from) and 3 (copy to).
align 8
move_gdt:
    times 16 db 0               ; entries 0, 1: for the BIOS
move_src:                       ; entry 2: the bounce buffer
    dw 0xFFFF                   ; limit: 64 KiB
    dw BOUNCE & 0xFFFF          ; base bits 0-15
    db (BOUNCE >> 16) & 0xFF    ; base bits 16-23
    db 0x93                     ; access: present, data, writable
    db 0                        ; limit bits 16-19, flags
    db (BOUNCE >> 24) & 0xFF    ; base bits 24-31
move_dst:                       ; entry 3: the destination (patched by load)
    dw 0xFFFF
    dw 0
    db 0
    db 0x93
    db 0
    db 0
    times 16 db 0               ; entries 4, 5: for the BIOS

align 4
dap:                            ; Disk Address Packet for int 0x13 / ah=0x42
    db 0x10                     ; size of this packet (16 bytes)
    db 0
dap_count:
    dw 0                        ; number of sectors (set by read_bounce)
dap_off:
    dw 0                        ; destination offset
dap_seg:
    dw 0                        ; destination segment
dap_lba:
    dq 0                        ; starting sector (set by read_bounce)

boot_drive:     db 0
prog_done:      dd 0            ; sectors copied, for the progress bar
prog_total:     dd 0
hold_tick:      dw 0
hold_spins:     dd 0            ; a cap, in case the BIOS clock never ticks
msg_disk_error: db 13, 10, "MyOS: disk read error!", 13, 10, 0
msg_move_error: db 13, 10, "MyOS: copy above 1 MiB failed!", 13, 10, 0

; Stage 2 must fit in its STAGE2_SECTORS; if not, this count goes negative
; and nasm stops with an error.
times STAGE2_SECTORS * 512 - ($ - $$) db 0
