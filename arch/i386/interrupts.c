/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * interrupts.c — the IDT, and dispatching interrupts to handlers
 *
 * The IDT (Interrupt Descriptor Table) is to interrupts what the GDT is to
 * segments: a table the CPU consults by itself. Entry N says where to jump
 * when interrupt/exception vector N happens. Each 8-byte entry ("gate")
 * holds the handler address, the code segment to run it in, and flags.
 *
 * Every vector lands in common_stub (isr.asm), which calls
 * interrupt_dispatch() below with all registers saved:
 *   vectors 0-31   CPU exceptions  -> kernel: print what happened and stop
 *                                     user program: kill just that program
 *   vectors 32-47  hardware IRQs   -> the driver's handler, then EOI
 *   vector 128     int 0x80        -> Linux system call (syscall.c)
 */
#include <gdt.h>
#include <apic.h>
#include <interrupts.h>
#include <kprintf.h>
#include <pic.h>
#include <stddef.h>
#include <mm.h>
#include <proc.h>
#include <vfs.h>
#include <sched.h>
#include <serial.h>
#include <string.h>
#include <syscall.h>
#include <thread.h>

struct idt_gate {
    uint16_t offset_low;    /* handler address bits 0-15 */
    uint16_t selector;      /* code segment to run the handler in */
    uint8_t  zero;
    uint8_t  type_attr;     /* present, privilege level, gate type */
    uint16_t offset_high;   /* handler address bits 16-31 */
} __attribute__((packed));

struct idt_ptr {
    uint16_t size;
    uint32_t base;
} __attribute__((packed));

static struct idt_gate idt[256];
#define IRQ_SHARE 4
static isr_fn_t irq_handlers[NR_IRQS][IRQ_SHARE];

extern const uint32_t isr_stub_table[32 + NR_IRQS];    /* from isr.asm */
extern char isr_syscall[];
extern char isr_spurious[];

static void double_fault(void);

/* type_attr 0x8E = present | ring 0 | 32-bit interrupt gate.
   An "interrupt gate" turns interrupts off while the handler runs, so a
   handler is never interrupted halfway by another IRQ.
   The ring ("DPL", bits 5-6) is who may trigger the vector with an `int`
   instruction. Ring 0 for everything except int 0x80, int3 and into, which
   are ring 3 (0xEE) as on Linux: otherwise a user program could fake a page
   fault or timer tick, and its int3 would be a protection fault, not the
   SIGTRAP debuggers and Chromium's CHECKs expect. */
static void set_gate(int vector, uint32_t handler, int user_allowed)
{
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].zero        = 0;
    idt[vector].type_attr   = user_allowed ? 0xEE : 0x8E;
    idt[vector].offset_high = handler >> 16;
}

void idt_init(void)
{
    for (int v = 0; v < 32 + NR_IRQS; v++)
        set_gate(v, isr_stub_table[v], v == 3 || v == 4);   /* int3, into: SIGTRAP, SIGSEGV */
    set_gate(0x80, (uint32_t)(uintptr_t)isr_syscall, 1);
    set_gate(0xFF, (uint32_t)(uintptr_t)isr_spurious, 0);
    /* The double fault: a task gate (type 5), so it runs on a stack of its
       own even when the kernel stack is the thing that broke (gdt.c). */
    tss_double_fault_init(double_fault);
    idt[8] = (struct idt_gate){ 0, GDT_DF_TSS, 0, 0x85, 0 };
    /* The other vectors stay empty (not present). Using one would cause a
       General Protection fault, which we do catch. */

    struct idt_ptr ptr = { sizeof idt - 1, (uint32_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ptr));

    pic_init();
}

/* PCI devices share interrupt lines, so a line has up to IRQ_SHARE
   handlers; each one checks whether its own device asked. */
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
    kprintf("irq %d: too many devices share it\n", irq);
}

static const char *const exception_names[32] = {
    "Divide error", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 floating-point error", "Alignment check", "Machine check", "SIMD floating-point error",
    "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security", "Reserved",
};

/* Which Linux signal a CPU exception becomes in a user program. */
static int exception_signal(uint32_t vector)
{
    switch (vector) {
    case 0: case 16: case 19: return 8;     /* SIGFPE: divide, FPU, SSE error */
    case 3:                   return 5;     /* SIGTRAP: breakpoint */
    case 6:                   return 4;     /* SIGILL: invalid opcode */
    default:                  return 11;    /* SIGSEGV */
    }
}

/* A fault in a user program: kill just that program, like Linux's SIGSEGV.
   The kernel is fine — the program did something it isn't allowed to.
   (Programs catching these signals comes with signals, step 8f.)
   noinline: its name buffer gets a stack canary (Makefile: KERNEL_SSP);
   inlined, every interrupt and int 0x80 would pay for that check. */
static __attribute__((noinline)) int user_exception(struct regs *r, uint32_t cr2)
{
    struct thread *t = thread_current();
    kprintf("%s: killed by CPU exception %lu (%s) at EIP %08lx",
            t->name, r->vector, exception_names[r->vector], r->eip);
    if (r->vector == 14)
        kprintf(", %s of %08lx (%s)", r->err_code & 2 ? "write" : "read", cr2,
                r->err_code & 1 ? "not allowed" : "not mapped");
    kprintf(" EAX=%08lx EBX=%08lx ESP=%08lx GS=%04lx",
            r->eax, r->ebx, r->user_esp, r->gs);
    struct mm *mm = mm_current();
    if (mm) {
        kprintf(" (%d areas)", mm->nareas);
        for (int i = 0; i < mm->nareas; i++) {
            struct vma *v = &mm->area[i];
            if (r->eip < v->start || r->eip >= v->end || !v->file || !v->file->name)
                continue;
            char n[40];
            uint32_t len = v->file->name_len < 39 ? v->file->name_len : 39;
            memcpy(n, v->file->name, len);
            n[len] = '\0';
            kprintf(" in %s+%lx", n, (unsigned long)(r->eip - v->start + v->file_off));
            break;
        }
    }
    kprintf("\n");
    int sig = exception_signal(r->vector);
    struct process *p = proc_current();
    uint32_t h = p->sig_action[sig - 1].handler;
    if (h != SIG_DFL && h != SIG_IGN && !(t->sig_blocked & SIGBIT(sig))) {
        t->sig_pending |= SIGBIT(sig);  /* its handler runs on the way out */
        t->sig_check = 1;
        return 1;
    }
    proc_exit(W_SIGNALED(sig));     /* never returns */
    return 0;
}

/* The exception table (uaccess.asm; cpu.h's rdmsr_safe): instructions that
   may fault, and where to continue if they do. */
struct ex_entry { uint32_t insn, fixup; };
extern const struct ex_entry __start_ex_table[], __stop_ex_table[];

static int fixup_exception(struct regs *r)
{
    for (const struct ex_entry *e = __start_ex_table; e < __stop_ex_table; e++) {
        if (e->insn == r->eip) {
            r->eip = e->fixup;      /* `iret` resumes at the fixup: -EFAULT */
            return 1;
        }
    }
    return 0;
}

/* Who called whom: return addresses on the stack (no frame pointers, so a
   scan, as Linux does without them; some values are stale), from sp up to
   the top of the thread's own stack. */
static void stack_scan(uint32_t sp)
{
    extern char __text_start[], __text_end[], boot_stack_top[];
    struct thread *t = thread_current();
    uint32_t top = t && t->stack ? (uint32_t)t->stack + THREAD_STACK : (uint32_t)boot_stack_top;
    uint32_t bottom = t && t->stack ? (uint32_t)t->stack : top - 16384;
    if (sp < bottom || sp >= top)
        return;
    kprintf("In thread \"%s\"; called from (newest first):", t ? t->name : "?");
    int n = 0;
    for (const uint32_t *p = (const uint32_t *)sp; (uint32_t)p < top && n < 16; p++)
        if (*p >= (uint32_t)__text_start && *p < (uint32_t)__text_end)
            kprintf("%s%08lx", n++ % 6 ? " " : "\n  ", *p);
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
    kprintf("\n*** KERNEL STACK SMASHED: the function at %08lx overran a local array ***\n",
            (unsigned long)__builtin_return_address(0));
    uint32_t sp;
    __asm__ volatile ("mov %%esp, %0" : "=r"(sp));
    stack_scan(sp);
    kprintf("System halted. (Look it up with: i686-elf-addr2line -f -e build/kernel.elf ADDRESS)\n");
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* A CPU exception in the kernel is a bug: show everything we know, then stop
   (instead of the CPU giving up and rebooting — a "triple fault"). */
static void exception(struct regs *r)
{
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));   /* page faults: bad address */

    /* A page fault may just be memory that doesn't exist YET: a stack or
       heap page touched for the first time, or a copy-on-write page after
       fork. mm_fault() creates it, and the instruction runs again. This
       works for the kernel touching user memory too (copy_to_user). */
    if (r->vector == 14 && mm_fault(cr2, r->err_code) == 0)
        return;

    if (r->cs & 3) {                /* the CPU was in ring 3: a user program */
        user_exception(r, cr2);     /* kills it, or queues its handler */
        return;
    }
    if ((r->vector == 14 || r->vector == 13) && fixup_exception(r))
        return;                     /* a bad user pointer (page fault), or an MSR
                                       this CPU lacks (#GP): not a kernel bug */

    kprintf("\n*** CPU EXCEPTION %lu: %s (error code %lx) ***\n",
            r->vector, exception_names[r->vector], r->err_code);
    if (r->vector == 14) {
        /* Page fault: CR2 holds the address that was accessed, and the
           error code says how: bit 0 = page was present (so it's a
           permission problem), bit 1 = write, bit 2 = from user mode. */
        kprintf("%s %s of address %08lx (%s)\n",
                r->err_code & 4 ? "User" : "Kernel",
                r->err_code & 2 ? "write" : "read", cr2,
                r->err_code & 1 ? "permission denied" : "page not mapped");
    }
    kprintf("EIP=%08lx  CS=%04lx  EFLAGS=%08lx  CR2=%08lx\n", r->eip, r->cs, r->eflags, cr2);
    struct thread *over = thread_stack_guard(cr2);
    if (r->vector == 14 && over)
        kprintf("KERNEL STACK OVERFLOW: thread \"%s\" ran off the end of its %u-byte "
                "stack (stopped at its guard page)\n", over->name, (unsigned)THREAD_STACK);
    kprintf("EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("ESI=%08lx EDI=%08lx EBP=%08lx\n", r->esi, r->edi, r->ebp);
    /* In ring 0 the CPU pushed no ESP: the stack went on right after eflags. */
    stack_scan((uint32_t)&r->user_esp);
    kprintf("System halted. (Look up EIP with: i686-elf-addr2line -e build/kernel.elf %lx)\n",
            r->eip);
    serial_flush();                 /* interrupts are off: send it all now */
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* The double-fault task (gdt.c): on its own stack, interrupts off. The
   code that faulted left its registers in the kernel's TSS. Never returns:
   a double fault in the kernel is always a bug. */
static void double_fault(void)
{
    uint32_t cr2, eip, esp;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    tss_saved_state(&eip, &esp);
    struct thread *t = thread_current(), *over = thread_stack_guard(cr2);
    kprintf("\n*** CPU EXCEPTION 8: Double fault ***\n");
    kprintf("EIP=%08lx ESP=%08lx CR2=%08lx in thread \"%s\"\n", (unsigned long)eip,
            (unsigned long)esp, (unsigned long)cr2, t ? t->name : "?");
    if (over)
        kprintf("KERNEL STACK OVERFLOW: thread \"%s\" ran off the end of its %u-byte "
                "stack (stopped at its guard page)\n", over->name, (unsigned)THREAD_STACK);
    /* The deepest calls: return addresses from the bottom of the stack. */
    extern char __text_start[], __text_end[];
    if (t && t->stack) {
        kprintf("called from (newest first):");
        int n = 0;
        for (const uint32_t *p = t->stack; (uint8_t *)p < (uint8_t *)t->stack + THREAD_STACK
             && n < 16; p++)
            if (*p >= (uint32_t)__text_start && *p < (uint32_t)__text_end)
                kprintf("%s%08lx", n++ % 6 ? " " : "\n  ", (unsigned long)*p);
        kprintf("\n");
    }
    kprintf("System halted. (Look up EIP with: i686-elf-addr2line -e build/kernel.elf %lx)\n",
            (unsigned long)eip);
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Called from common_stub in isr.asm for every interrupt. */
void interrupt_dispatch(struct regs *r)
{
    if (r->vector < 32) {
        exception(r);
        SIGNAL_CHECK(r);            /* e.g. a handled SIGSEGV */
        return;
    }

    if (r->vector == 0x80) {
        /* Linux i386 convention: number in EAX, arguments in EBX, ECX, EDX,
           ESI, EDI, EBP; the result goes back in EAX. */
        thread_current()->uregs = r;    /* fork/execve need the registers */
        uint32_t nr = r->eax;
        r->eax = syscall_dispatch(nr, r->ebx, r->ecx, r->edx,
                                  r->esi, r->edi, r->ebp);
        if (r->eax == (uint32_t)-ERESTARTSYS)
            signal_restart_note(nr);
        sched_preempt_check();      /* the slot may have ended meanwhile */
        SIGNAL_CHECK(r);            /* back to user mode: deliver signals */
        return;
    }

    int irq = r->vector - PIC_IRQ_BASE;
    cpu_idle_end();                 /* if the CPU was halted, it woke up now */
    if (irq < IRQ_MSI_FIRST && pic_is_spurious(irq))
        return;
    for (int k = 0; k < IRQ_SHARE && irq_handlers[irq][k]; k++)
        irq_handlers[irq][k](r);
    if (irq < IRQ_MSI_FIRST)
        pic_send_eoi(irq);
    else
        lapic_eoi();                /* an MSI is acknowledged at the local APIC */

    /* If the timer said "time's up", switch threads now. This must come
       after the EOI: the PIC won't send another tick until it gets one. */
    sched_preempt_check();
    if (r->cs & 3)
        SIGNAL_CHECK(r);            /* e.g. Ctrl-C for a busy program */
}
