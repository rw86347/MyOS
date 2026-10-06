#ifndef ARCH_I386_INTERRUPTS_H
#define ARCH_I386_INTERRUPTS_H

#include <stdint.h>

/*
 * The CPU state saved when an interrupt happens, exactly as isr.asm and the
 * CPU pushed it. Lowest address (pushed last) first.
 */
struct regs {
    uint32_t gs, fs, es, ds;                          /* pushed by common_stub */
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;  /* pushad */
    uint32_t vector, err_code;                        /* pushed by the stub */
    uint32_t eip, cs, eflags;                         /* pushed by the CPU */
    uint32_t user_esp, user_ss;   /* pushed by the CPU ONLY when coming from
                                     ring 3 (check cs & 3 before using them) */
};

typedef void (*isr_fn_t)(struct regs *r);

/* Fill in the IDT and load it. Call once, early. */
void idt_init(void);

/* Run `handler` whenever hardware IRQ `irq` (0-15) fires, and unmask it. */
/* IRQ numbers: 0-15 are the 8259's lines, 16-31 MSI/MSI-X vectors handed
   out by apic.c. Either way IRQ n arrives as CPU vector 32 + n. */
#define IRQ_MSI_FIRST 16
#define NR_IRQS       32

void irq_install(int irq, isr_fn_t handler);

static inline void interrupts_enable(void)  { __asm__ volatile ("sti"); }
static inline void interrupts_disable(void) { __asm__ volatile ("cli"); }

/* Turn interrupts off and return the old EFLAGS, so irq_restore() can put
   them back exactly as they were (on or off). Use around code that must not
   be interrupted halfway, like switching threads. */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    if (flags & (1 << 9))           /* bit 9 of EFLAGS = IF, interrupts enabled */
        __asm__ volatile ("sti" : : : "memory");
}

/* Sleep the CPU until the next interrupt arrives (saves power, too). */
static inline void cpu_wait(void) { __asm__ volatile ("hlt"); }

#endif
