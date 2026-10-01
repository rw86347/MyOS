; =============================================================================
; mbr.asm — MyOS Master Boot Record (stage 1 bootloader)
; =============================================================================
;
; What happens at power-on:
;   1. The BIOS runs its self-test, then reads the FIRST 512-byte sector of the
;      boot disk (the "Master Boot Record") into memory at address 0x7C00.
;   2. It checks that the last two bytes are 0x55 0xAA (the "boot signature").
;   3. It jumps to 0x7C00 with the CPU in 16-bit "real mode" — the same mode an
;      8086 from 1978 ran in. DL holds the BIOS drive number we booted from.
;
; Our job in these 512 bytes:
;   a. Set up segment registers and a stack.
;   b. Use the BIOS (int 0x13) to read the kernel from the disk sectors right
;      after this one, into memory at KERNEL_LOAD.
;   c. Enable the A20 line (so we can address memory above 1 MiB).
;   d. Load a GDT and switch the CPU into 32-bit "protected mode".
;   e. Jump to the kernel.
;
; Disk layout of build/myos.img:
;   sector 0 (LBA 0)        : this MBR
;   sectors 1..N (LBA 1..N) : kernel.bin (flat binary, linked to run at KERNEL_LOAD)
;
; KERNEL_SECTORS (N) is passed in by the Makefile with -DKERNEL_SECTORS=...
; =============================================================================

[bits 16]                       ; the CPU starts in 16-bit real mode
[org 0x7C00]                    ; the BIOS loads us here, so addresses start here

KERNEL_LOAD     equ 0x10000     ; physical address where the kernel is loaded (64 KiB)
KERNEL_STACK    equ 0x90000     ; initial kernel stack top (grows down; free RAM below 640K)

%ifndef KERNEL_SECTORS
  %error "KERNEL_SECTORS must be defined (the Makefile does this)"
%endif
%if KERNEL_SECTORS > 127
  ; Many old BIOSes can only read 127 sectors per int 0x13 call. When the kernel
  ; grows past ~63 KiB we will need a loop here (or a stage-2 loader).
  %error "Kernel too big for single-read MBR loader (max 127 sectors)"
%endif

start:
    ; --- a. Segments and stack ------------------------------------------------
    ; In real mode an address is segment*16 + offset. Some BIOSes jump to us as
    ; 0x07C0:0x0000 instead of 0x0000:0x7C00, so the far jump below forces CS=0.
    jmp 0x0000:.flush_cs
.flush_cs:
    cli                         ; no interrupts while we change the stack
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stack grows down from just below our code
    sti

    mov [boot_drive], dl        ; remember which drive the BIOS booted us from

    mov si, msg_loading
    call print16

    ; --- b. Load the kernel with BIOS int 0x13, function 0x42 ----------------
    ; Function 0x42 ("extended read") takes a Disk Address Packet (DAP) that
    ; describes how many sectors to read, from which LBA (sector number), and
    ; into which segment:offset. LBA addressing is much simpler than the old
    ; cylinder/head/sector scheme and is supported by BIOSes from ~1998 on.
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error               ; carry flag set = the read failed

    ; --- c. Enable the A20 line ----------------------------------------------
    ; Historical quirk: the 8086 could only address 1 MiB, and addresses
    ; "wrapped around". To stay compatible, PCs keep address line 20 forced to
    ; 0 until software turns it on. "Fast A20" via port 0x92 works on nearly
    ; all machines from the Pentium era onward.
    in al, 0x92
    or al, 2                    ; bit 1 = A20 enable
    and al, 0xFE                ; never write bit 0 (it resets the machine!)
    out 0x92, al

    ; --- d. Switch to protected mode ------------------------------------------
    cli                         ; BIOS interrupt handlers are 16-bit: keep them off
    lgdt [gdt_descriptor]       ; tell the CPU where our GDT is
    mov eax, cr0
    or eax, 1                   ; CR0 bit 0 = PE (Protection Enable)
    mov cr0, eax
    ; We are now in protected mode, but CS still holds a real-mode value.
    ; A far jump reloads CS with our 32-bit code segment selector (0x08).
    jmp CODE_SEG:protected_mode

disk_error:
    mov si, msg_disk_error
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
    mov esp, KERNEL_STACK

    ; --- e. Jump to the kernel's entry point (_start in arch/i386/entry.asm) --
    jmp KERNEL_LOAD

; -----------------------------------------------------------------------------
; GDT — Global Descriptor Table
; -----------------------------------------------------------------------------
; In protected mode, segment registers no longer hold "address / 16". They
; hold a *selector*: an index into this table. Each 8-byte entry (descriptor)
; describes a segment's base address, size limit and permissions.
;
; We use the "flat model": one code and one data segment, both starting at 0
; and covering all 4 GiB. That effectively turns segmentation off, which is
; what every modern OS does (paging does the real memory protection later).
; The kernel will install its own GDT later (roadmap step 2).
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

; -----------------------------------------------------------------------------
; Data
; -----------------------------------------------------------------------------
align 4
dap:                            ; Disk Address Packet for int 0x13 / ah=0x42
    db 0x10                     ; size of this packet (16 bytes)
    db 0                        ; reserved
    dw KERNEL_SECTORS           ; number of sectors to read
    dw 0x0000                   ; destination offset  } 0x1000:0x0000
    dw KERNEL_LOAD >> 4         ; destination segment } = physical 0x10000
    dq 1                        ; starting LBA (sector 1, right after the MBR)

boot_drive:     db 0
msg_loading:    db "MyOS: loading kernel...", 13, 10, 0
msg_disk_error: db "MyOS: disk read error!", 13, 10, 0

; -----------------------------------------------------------------------------
; Pad to 510 bytes and add the boot signature
; -----------------------------------------------------------------------------
; (Bytes 446-509 are where a partition table lives on a real disk. We don't
; need one for QEMU; we'll add one before booting from USB on real hardware.)
times 510 - ($ - $$) db 0
dw 0xAA55                       ; stored little-endian as 0x55, 0xAA
