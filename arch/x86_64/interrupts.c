/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#include <gdt.h>
#include <apic.h>
#include <interrupts.h>
#include <kprintf.h>
#include <pic.h>
#include <mm.h>
#include <proc.h>
#include <sched.h>
#include <serial.h>
#include <signal.h>
#include <string.h>
#include <sched.h>
#include <syscall.h>
#include <thread.h>
#include <paging.h>
#include <uaccess.h>
#include <vfs.h>


struct idt_gate64 {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_gate64 idt[256];
#define IRQ_SHARE 4
static isr_fn_t irq_handlers[NR_IRQS][IRQ_SHARE];

extern const uint64_t isr_stub_table[32 + NR_IRQS];
extern char isr_spurious[];

/* type_attr 0x8E: present, ring 0, interrupt gate. A program may only
   use `int N` on a ring-3 gate (0xEE); the breakpoint (int3, vector 3) is
   one, as on Linux, so a program's int3 is a SIGTRAP — Chromium's CHECK
   failures and debuggers' breakpoints — not a protection fault. */
static void set_gate(int vector, uint64_t handler, int user_allowed)
{
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = user_allowed ? 0xEE : 0x8E;
    idt[vector].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (uint32_t)(handler >> 32);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    for (int v = 0; v < 32 + NR_IRQS; v++)
        set_gate(v, isr_stub_table[v], v == 3);
    idt[8].ist = 1;                     /* the double fault: on its own stack (gdt.c) */
    set_gate(0xFF, (uint64_t)(uintptr_t)isr_spurious, 0);
    struct idt_ptr64 ptr = { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ptr));
    pic_init();
}

void irq_install(int irq, isr_fn_t handler)
{
    if (irq < 0 || irq >= NR_IRQS)
        return;
    for (int k = 0; k < IRQ_SHARE; k++)
        if (!irq_handlers[irq][k] || irq_handlers[irq][k] == handler) {
            irq_handlers[irq][k] = handler;
            if (irq < IRQ_MSI_FIRST)
                pic_unmask(irq);        /* an MSI has no line to unmask */
            return;
        }
}

static const char *const exception_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor segment",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection",
    "Page fault", "Reserved", "x87 error", "Alignment check", "Machine check",
    "SIMD error", "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor", "VMM", "Security",
    "Reserved",
};

struct ex_entry { uint64_t insn, fixup; };      /* uaccess.asm EX_ENTRY, cpu.h rdmsr_safe */
extern const struct ex_entry __start_ex_table[], __stop_ex_table[];

static int fixup_exception(struct regs *r)
{
    uint64_t rip = regs_ip(r);
    for (const struct ex_entry *e = __start_ex_table; e < __stop_ex_table; e++) {
        if (e->insn == rip) {
            regs_set_ip(r, e->fixup);
            return 1;
        }
    }
    return 0;
}

/* Which Linux signal a CPU exception becomes in a user program. */
static int exception_signal(uint64_t vector)
{
    switch (vector) {
    case 0: case 16: case 19: return SIGFPE;    /* divide, FPU, SSE error */
    case 3:                   return SIGTRAP;   /* breakpoint */
    case 6:                   return SIGILL;    /* invalid opcode */
    default:                  return SIGSEGV;
    }
}

/* A fault in a user program. If it handles the signal, the handler runs on
   the way out (like Linux); otherwise the program dies, and the kernel log
   says where. noinline: its name buffer gets a stack canary (Makefile:
   KERNEL_SSP); inlined, every interrupt would pay for that check. */
static __attribute__((noinline)) void user_exception(struct regs *r, uint64_t cr2)
{
    struct thread *t = thread_current();
    int sig = exception_signal(r->vector);
    user_addr_t h = t->proc->sig_action[sig - 1].handler;
    int handled = h != SIG_DFL && h != SIG_IGN && !(t->sig_blocked & SIGBIT(sig));
    /* A handled fault is logged too, like i386 does: a crash reporter
       (Chromium's crashpad) catches the fault, then ends the program with
       a plain kill, and this is the only trace of where it went wrong.
       Only the first 50: some programs fault on purpose (V8's WebAssembly
       bounds checks), and the log must not become the work. */
    static uint32_t handled_logged;
    if (handled && handled_logged >= 50) {
        t->sig_pending |= SIGBIT(sig);
        t->sig_check = 1;
        return;
    }
    handled_logged += (uint32_t)handled;
    kprintf("%s[%d]: %s CPU exception %lu (%s) at RIP %lx", t->name, t->proc->pid,
            handled ? "its handler gets" : "killed by",
            r->vector, exception_names[r->vector < 32 ? r->vector : 0], regs_ip(r));
    if (r->vector == 14)
        kprintf(", %s of %lx (%s)", r->err_code & 2 ? "write" : "read", cr2,
                r->err_code & 1 ? "not allowed" : "not mapped");
    /* Which file the instruction is in, and where — Linux's "in libfoo.so"
       — so the crash can be looked up: objdump -d at that file offset. */
    struct mm *mm = mm_current();
    for (int k = 0; mm && k < mm->nareas; k++) {
        struct vma *v = &mm->area[k];
        if (regs_ip(r) < v->start || regs_ip(r) >= v->end || !v->file || !v->file->name)
            continue;
        char nm[48];
        uint32_t n = v->file->name_len < sizeof nm - 1 ? v->file->name_len : sizeof nm - 1;
        memcpy(nm, v->file->name, n);
        nm[n] = 0;
        kprintf(" in %s at file offset %lx", nm,
                (unsigned long)(v->file_off + (regs_ip(r) - v->start)));
        break;
    }
    kprintf(" sp %lx\n", (unsigned long)r->rsp);
    /* Who called it: words on the user stack that point into a mapped
       file's code, as file + offset — return addresses, mostly (some are
       stale). With the program's symbols, that is the call chain. */
    for (int k = 0, shown = 0; mm && k < 400 && shown < 14; k++) {
        uint64_t w;
        if (copy_from_user(&w, (const void *)(uintptr_t)(r->rsp + (uint64_t)k * 8), 8) < 0)
            break;
        for (int j = 0; j < mm->nareas; j++) {
            struct vma *v = &mm->area[j];
            if (w < v->start || w >= v->end || !(v->flags & VM_EXEC) || !v->file || !v->file->name)
                continue;
            char nm[32];
            uint32_t n = v->file->name_len < sizeof nm - 1 ? v->file->name_len : sizeof nm - 1;
            memcpy(nm, v->file->name, n);
            nm[n] = 0;
            kprintf("  stack+%x: %s+%lx\n", k * 8, nm,
                    (unsigned long)(v->file_off + (w - v->start)));
            shown++;
            break;
        }
    }
#ifdef PT_AUDIT
    {   /* the registers, and the memory the bad pointer came from */
        kprintf("  rax %lx rbx %lx rcx %lx rdx %lx rsi %lx rdi %lx rbp %lx\n",
                r->rax, r->rbx, r->rcx, r->rdx, r->rsi, r->rdi, r->rbp);
        kprintf("  r8 %lx r9 %lx r10 %lx r11 %lx r12 %lx r13 %lx r14 %lx r15 %lx\n",
                r->r8, r->r9, r->r10, r->r11, r->r12, r->r13, r->r14, r->r15);
        uint64_t at[4] = { r->rdi, r->rsi, r->rbx, r->r12 };
        const char *nm[4] = { "rdi", "rsi", "rbx", "r12" };
        for (int k = 0; k < 4; k++) {
            uint64_t w[6];
            if (at[k] < 0x1000 || at[k] >= USER_ADDR_END ||
                copy_from_user(w, (const void *)(uintptr_t)(at[k] & ~7ull), sizeof w) < 0)
                continue;
            kprintf("  [%s] %lx: %lx %lx %lx %lx %lx %lx  (frame %lx)\n", nm[k], at[k],
                    w[0], w[1], w[2], w[3], w[4], w[5],
                    (unsigned long)vmm_translate(at[k] & ~0xFFFull));
        }
        vmm_audit();
    }
#endif
#ifdef SYS_TRACE
    thread_trace_process(t);
#endif
    if (handled) {
        t->sig_pending |= SIGBIT(sig);
        t->sig_check = 1;
        return;
    }
    proc_exit(W_SIGNALED(sig));
}

/* A kernel crash: who called whom. The kernel has no frame pointers, so,
   as Linux does without them, the stack is scanned for values that point
   into the kernel's code: return addresses, mostly — some are stale, so
   read it from the top. Only within the thread's own stack, so the scan
   itself can't fault. Look them up with x86_64-elf-addr2line -f -e
   kernel.elf ADDRESSES. */
extern char __text_start[], __text_end[], boot_stack_top[];

static void stack_scan(uint64_t sp)
{
    struct thread *t = thread_current();
    uint64_t top = t && t->stack ? (uint64_t)t->stack + THREAD_STACK : (uint64_t)boot_stack_top;
    uint64_t bottom = t && t->stack ? (uint64_t)t->stack : top - 16384;
    if (sp < bottom && top - bottom <= 16384)
        sp = bottom;                    /* it ran off the end: the deepest calls */
    if (sp & 7 || sp >= top || sp < bottom)
        return;
    kprintf("in thread \"%s\"; called from (newest first):", t ? t->name : "?");
    int n = 0;
    for (const uint64_t *p = (const uint64_t *)sp; (uint64_t)p < top && n < 16; p++)
        if (*p >= (uint64_t)__text_start && *p < (uint64_t)__text_end)
            kprintf("%s%lx", n++ % 4 ? " " : "\n  ", *p);
    kprintf("\n");
}

/* A protected function (Makefile: KERNEL_SSP) found its stack canary
   changed on its way out: something wrote past the end of one of its
   local arrays, and maybe over its return address too. Stop here instead
   of returning there. The address printed is in that function. */
void __stack_chk_fail(void) __attribute__((noreturn));
void __stack_chk_fail(void)
{
    __asm__ volatile ("cli");
    kprintf("\n*** KERNEL STACK SMASHED: the function at %lx overran a local array ***\n",
            (uint64_t)__builtin_return_address(0));
    uint64_t sp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(sp));
    stack_scan(sp);
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

static void exception(struct regs *r)
{
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    if (r->vector == 14 && mm_fault(cr2, (uint32_t)r->err_code) == 0)
        return;
    if (regs_user_mode(r)) {
        user_exception(r, cr2);
        return;
    }
    if ((r->vector == 14 || r->vector == 13) && fixup_exception(r))
        return;                     /* a bad user pointer (page fault), or an MSR
                                       this CPU lacks (#GP): not a kernel bug */
    kprintf("\n*** CPU EXCEPTION %lu: %s ***\n", r->vector,
            exception_names[r->vector < 32 ? r->vector : 0]);
    kprintf("RIP=%016lx CR2=%016lx error code %lx\n", regs_ip(r), cr2, r->err_code);
    struct thread *over = thread_stack_guard(cr2);
    if ((r->vector == 8 || r->vector == 14) && over)
        kprintf("KERNEL STACK OVERFLOW: thread \"%s\" ran off the end of its %u-byte "
                "stack (stopped at its guard page)\n", over->name, (unsigned)THREAD_STACK);
    stack_scan(regs_sp(r));
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* From syscall_entry.asm. Linux x86_64 convention: the number in RAX,
   arguments in RDI, RSI, RDX, R10, R8, R9 (R10, not RCX: `syscall` uses RCX
   for the return address); the result goes back in RAX. */
void syscall_entry_c(struct regs *r)
{
    thread_current()->uregs = r;        /* fork/execve need the registers */
    uint64_t nr = r->rax;
    r->rax = nr <= 0xFFFFFFFF ? syscall_dispatch((uint32_t)nr, r->rdi, r->rsi, r->rdx,
                                                 r->r10, r->r8, r->r9)
                              : -ENOSYS;
    if (r->rax == (uint64_t)-ERESTARTSYS)
        signal_restart_note((uint32_t)nr);
    sched_preempt_check();              /* the slot may have ended meanwhile */
    SIGNAL_CHECK(r);                    /* back to user mode: deliver signals */
}

void interrupt_dispatch(struct regs *r)
{
    if (r->vector < 32) {
        exception(r);
        if (regs_user_mode(r))
            SIGNAL_CHECK(r);            /* e.g. a handled SIGSEGV */
        return;
    }
    int irq = (int)r->vector - PIC_IRQ_BASE;
    cpu_idle_end();
    if (irq < IRQ_MSI_FIRST && pic_is_spurious(irq))
        return;
    for (int k = 0; k < IRQ_SHARE && irq_handlers[irq][k]; k++)
        irq_handlers[irq][k](r);
    if (irq < IRQ_MSI_FIRST)
        pic_send_eoi(irq);
    else
        lapic_eoi();                    /* an MSI is acknowledged at the local APIC */
    sched_preempt_check();
    if (regs_user_mode(r))
        SIGNAL_CHECK(r);                /* e.g. Ctrl-C for a busy program */
}
