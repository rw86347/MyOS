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
 *   vectors 0-31   CPU exceptions  -> print what happened and stop
 *   vectors 32-47  hardware IRQs   -> the driver's handler, then EOI
 */
#include <gdt.h>
#include <interrupts.h>
#include <kprintf.h>
#include <pic.h>
#include <stddef.h>

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
static irq_handler_t irq_handlers[16];

extern const uint32_t isr_stub_table[48];    /* from isr.asm */

/* type_attr 0x8E = present | ring 0 | 32-bit interrupt gate.
   An "interrupt gate" turns interrupts off while the handler runs, so a
   handler is never interrupted halfway by another IRQ. */
static void set_gate(int vector, uint32_t handler)
{
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].zero        = 0;
    idt[vector].type_attr   = 0x8E;
    idt[vector].offset_high = handler >> 16;
}

void idt_init(void)
{
    for (int v = 0; v < 48; v++)
        set_gate(v, isr_stub_table[v]);
    /* Vectors 48-255 stay empty (not present). Using one would cause a
       General Protection fault, which we do catch. */

    struct idt_ptr ptr = { sizeof idt - 1, (uint32_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(ptr));

    pic_init();
}

void irq_install(int irq, irq_handler_t handler)
{
    irq_handlers[irq] = handler;
    pic_unmask(irq);
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

/* A CPU exception in the kernel is a bug: show everything we know, then stop
   (instead of the CPU giving up and rebooting — a "triple fault"). Later,
   exceptions caused by user programs will kill just that program (SIGSEGV...). */
static void exception(struct regs *r)
{
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));   /* page faults: bad address */

    kprintf("\n*** CPU EXCEPTION %lu: %s (error code %lx) ***\n",
            r->vector, exception_names[r->vector], r->err_code);
    kprintf("EIP=%08lx  CS=%04lx  EFLAGS=%08lx  CR2=%08lx\n", r->eip, r->cs, r->eflags, cr2);
    kprintf("EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("ESI=%08lx EDI=%08lx EBP=%08lx\n", r->esi, r->edi, r->ebp);
    kprintf("System halted. (Look up EIP with: i686-elf-addr2line -e build/kernel.elf %lx)\n",
            r->eip);
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Called from common_stub in isr.asm for every interrupt. */
void interrupt_dispatch(struct regs *r)
{
    if (r->vector < 32) {
        exception(r);
        return;
    }

    int irq = r->vector - PIC_IRQ_BASE;
    if (pic_is_spurious(irq))
        return;
    if (irq_handlers[irq])
        irq_handlers[irq](r);
    pic_send_eoi(irq);
}
