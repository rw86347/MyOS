; =============================================================================
; switch.asm — switch the CPU from one kernel thread to another
; =============================================================================
; A paused thread is nothing more than its stack. Everything needed to resume
; it is pushed there, and the only thing we remember elsewhere is ESP.
;
; Which registers must be saved? The C calling convention (cdecl) says a
; called function may trash EAX, ECX and EDX — the caller already saved them
; if it cared. EBX, ESI, EDI and EBP must come back unchanged. So, from the
; point of view of the C code calling switch_context(), saving those four plus
; ESP is a complete snapshot. EIP is already on the stack: it's the return
; address that `call switch_context` pushed.
;
; A paused thread's stack therefore looks like this (top = lowest address):
;
;     saved ESP -> | EDI |
;                  | ESI |
;                  | EBX |
;                  | EBP |
;                  | EIP |  where `ret` will continue in that thread
;
; A brand-new thread gets a fake frame of the same shape (thread.c), whose
; EIP is thread_trampoline below — so "resuming" it starts it.
; =============================================================================

[bits 32]
section .text

; void switch_context(uint32_t *save_esp, uint32_t new_esp);
;   Save the current thread's registers on its stack, store its ESP in
;   *save_esp, then load new_esp and resume that thread.
global switch_context
switch_context:
    mov eax, [esp + 4]          ; eax = save_esp (where to store our ESP)
    mov edx, [esp + 8]          ; edx = new_esp  (the thread to resume)

    push ebp                    ; snapshot of the current thread
    push ebx
    push esi
    push edi
    mov [eax], esp              ; remember where it is

    mov esp, edx                ; <-- the switch: now on the other stack

    pop edi                     ; restore the other thread's registers
    pop esi
    pop ebx
    pop ebp
    ret                         ; continue wherever that thread left off

; First code a new thread runs. thread.c's fake frame puts the thread's
; function in EBX and its argument in ESI.
extern thread_exit
global thread_trampoline
thread_trampoline:
    sti                         ; we arrive from thread_yield, which turned
                                ; interrupts off; a new thread starts with them on
    push esi                    ; argument
    call ebx                    ; fn(arg)
    add esp, 4
    call thread_exit            ; fn returned: the thread is finished
                                ; (thread_exit never returns)
