#include <gdt.h>
#include <interrupts.h>
#include <kprintf.h>
#include <pic.h>
#include <mm.h>
#include <proc.h>
#include <sched.h>
#include <serial.h>
#include <string.h>
#include <sched.h>
#include <syscall64.h>
#include <thread.h>
#include <uaccess.h>

extern int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6);

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
static irq_handler_t irq_handlers[16][IRQ_SHARE];

extern const uint64_t isr_stub_table[48];

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
    for (int v = 0; v < 48; v++)
        set_gate(v, isr_stub_table[v]);
    struct idt_ptr64 ptr = { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ptr));
    pic_init();
}

void irq_install(int irq, irq_handler_t handler)
{
    for (int k = 0; k < IRQ_SHARE; k++)
        if (!irq_handlers[irq][k] || irq_handlers[irq][k] == handler) {
            irq_handlers[irq][k] = handler;
            pic_unmask(irq);
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

struct ex_entry { uint32_t insn, fixup; };
extern const struct ex_entry __start_ex_table[], __stop_ex_table[];

static int fixup_user_access(struct regs *r)
{
    uint32_t eip = (uint32_t)regs_ip(r);
    for (const struct ex_entry *e = __start_ex_table; e < __stop_ex_table; e++) {
        if (e->insn == eip) {
            regs_set_ip(r, e->fixup);
            return 1;
        }
    }
    return 0;
}

static void exception(struct regs *r)
{
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    if (r->vector == 14 && mm_fault((uint32_t)cr2, (uint32_t)r->err_code) == 0)
        return;
    if (regs_user_mode(r)) {
        kprintf("user fault %lu at %lx\n", r->vector, regs_ip(r));
        proc_exit(128 + 11);
        return;
    }
    if (r->vector == 14 && fixup_user_access(r))
        return;
    kprintf("\n*** CPU EXCEPTION %lu: %s ***\n", r->vector,
            exception_names[r->vector < 32 ? r->vector : 0]);
    kprintf("RIP=%016lx CR2=%016lx\n", regs_ip(r), cr2);
    serial_flush();
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void syscall_entry_c(struct regs *r)
{
    thread_current()->uregs = r;
    regs_set_ax(r, syscall_dispatch_x86_64(r->rax, r->rdi, r->rsi, r->rdx, r->r10,
                                           r->r8, r->r9));
    sched_preempt_check();
}

void interrupt_dispatch(struct regs *r)
{
    if (r->vector < 32) {
        exception(r);
        return;
    }
    int irq = (int)r->vector - PIC_IRQ_BASE;
    cpu_idle_end();
    if (pic_is_spurious(irq))
        return;
    for (int k = 0; k < IRQ_SHARE && irq_handlers[irq][k]; k++)
        irq_handlers[irq][k](r);
    pic_send_eoi(irq);
    sched_preempt_check();
}
