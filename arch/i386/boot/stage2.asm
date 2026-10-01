; =============================================================================
; stage2.asm — MyOS stage 2 bootloader
; =============================================================================
; The MBR loaded us at 0x7E00 and jumped here (real mode, DL = boot drive).
; Our jobs:
;   1. ask the BIOS for the memory map (E820) — only possible in real mode
;   2. enable the A20 line, so memory above 1 MiB is reachable
;   3. load the kernel to 1 MiB and the initramfs right after it
;   4. leave a note for the kernel at BOOTINFO (bootinfo.h)
;   5. switch to 32-bit protected mode and jump to the kernel
;
; The tricky part is 3. In real mode the BIOS disk call can only write into
; the first 1 MiB of memory. So we load in chunks: read 32 KiB into a
; "bounce buffer" below 1 MiB, then ask the BIOS to copy it above 1 MiB with
; int 0x15, AH=0x87 ("move extended memory block", in every PC BIOS since the
; 286). Repeat until done — any size, and it only uses the BIOS, so it works
; when booting from a USB stick or CD on real hardware too.
;
; Passed in by the Makefile with -D:
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

    mov si, msg_stage2
    call print16

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

    ; --- 3. Load the kernel and the initramfs -------------------------------------
    mov eax, KERNEL_LBA
    mov ecx, KERNEL_SECTORS
    mov edi, KERNEL_LOAD
    call load

    mov eax, INITRD_LBA
    mov ecx, INITRD_SECTORS
    mov edi, INITRD_LOAD
    call load

    ; --- 4. Tell the kernel where the initramfs is ------------------------------
    mov dword [BOOTINFO + 0], INITRD_LOAD
    mov dword [BOOTINFO + 4], INITRD_BYTES

    mov si, msg_done
    call print16

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
; (anywhere in memory, also above 1 MiB). Prints a dot per 32 KiB chunk.
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
    ; a. disk -> bounce buffer, BIOS int 0x13 AH=0x42 (as in the MBR)
    mov [dap_count], bx
    mov [dap_lba], eax
    pushad                      ; BIOS calls may change registers
    mov si, dap                 ; (pushad/popad don't touch the flags, so
    mov ah, 0x42                ;  the carry flag survives popad)
    mov dl, [boot_drive]
    int 0x13
    popad
    jc disk_error

    ; b. bounce buffer -> EDI, BIOS int 0x15 AH=0x87. The BIOS wants a little
    ;    GDT (move_gdt) whose entries 2 and 3 describe the source and the
    ;    destination; we patch the destination's base address = EDI.
    mov edx, edi
    mov [move_dst + 2], dx      ; base bits 0-15
    shr edx, 16
    mov [move_dst + 4], dl      ; base bits 16-23
    mov [move_dst + 7], dh      ; base bits 24-31
    pushad
    mov cx, bx
    shl cx, 8                   ; CX = 16-bit words to copy (sectors * 256)
    mov si, move_gdt            ; ES:SI = the table (ES = 0)
    mov ah, 0x87
    int 0x15
    mov ax, 0x0E2E              ; print '.' — progress you can watch
    int 0x10
    popad
    jc move_error

    ; c. next chunk
    add eax, ebx                ; next disk sector
    sub ecx, ebx                ; sectors left
    shl ebx, 9                  ; sectors -> bytes
    add edi, ebx                ; next destination
    jmp .next
.done:
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
    dw 0                        ; number of sectors (set by load)
    dw 0                        ; destination offset  } BOUNCE
    dw BOUNCE >> 4              ; destination segment }
dap_lba:
    dq 0                        ; starting sector (set by load)

boot_drive:     db 0
msg_stage2:     db "MyOS: stage 2 loading kernel + initramfs", 0
msg_done:       db " ok", 13, 10, 0
msg_disk_error: db 13, 10, "MyOS: disk read error!", 13, 10, 0
msg_move_error: db 13, 10, "MyOS: copy above 1 MiB failed!", 13, 10, 0

; Stage 2 must fit in its STAGE2_SECTORS; if not, this count goes negative
; and nasm stops with an error.
times STAGE2_SECTORS * 512 - ($ - $$) db 0
