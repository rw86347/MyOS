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
#include <uaccess.h>


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

static void set_gate(int vector, uint64_t handler)
{
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = 0x8E;
    idt[vector].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (uint32_t)(handler >> 32);
    idt[vector].reserved    = 0;
}

void idt_init(void)
{
    for (int v = 0; v < 32 + NR_IRQS; v++)
        set_gate(v, isr_stub_table[v]);
    set_gate(0xFF, (uint64_t)(uintptr_t)isr_spurious);
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

struct ex_entry { uint64_t insn, fixup; };      /* uaccess.asm EX_ENTRY */
extern const struct ex_entry __start_ex_table[], __stop_ex_table[];

static int fixup_user_access(struct regs *r)
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
   says where. */
static void user_exception(struct regs *r, uint64_t cr2)
{
    struct thread *t = thread_current();
    int sig = exception_signal(r->vector);
    user_addr_t h = t->proc->sig_action[sig - 1].handler;
    if (h != SIG_DFL && h != SIG_IGN && !(t->sig_blocked & SIGBIT(sig))) {
        t->sig_pending |= SIGBIT(sig);
        t->sig_check = 1;
        return;
    }
    kprintf("%s: killed by CPU exception %lu (%s) at RIP %lx", t->name, r->vector,
            exception_names[r->vector < 32 ? r->vector : 0], regs_ip(r));
    if (r->vector == 14)
        kprintf(", %s of %lx (%s)", r->err_code & 2 ? "write" : "read", cr2,
                r->err_code & 1 ? "not allowed" : "not mapped");
    kprintf("\n");
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
    if (sp & 7 || sp >= top || top - sp > 16384)
        return;
    kprintf("in thread \"%s\"; called from (newest first):", t ? t->name : "?");
    int n = 0;
    for (const uint64_t *p = (const uint64_t *)sp; (uint64_t)p < top && n < 16; p++)
        if (*p >= (uint64_t)__text_start && *p < (uint64_t)__text_end)
            kprintf("%s%lx", n++ % 4 ? " " : "\n  ", *p);
    kprintf("\n");
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
    if (r->vector == 14 && fixup_user_access(r))
        return;
    kprintf("\n*** CPU EXCEPTION %lu: %s ***\n", r->vector,
            exception_names[r->vector < 32 ? r->vector : 0]);
    kprintf("RIP=%016lx CR2=%016lx error code %lx\n", regs_ip(r), cr2, r->err_code);
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
