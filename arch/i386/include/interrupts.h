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
    /* (the CPU also pushes user ESP and SS when coming from ring 3 — step 5) */
};

typedef void (*irq_handler_t)(struct regs *r);

/* Fill in the IDT and load it. Call once, early. */
void idt_init(void);

/* Run `handler` whenever hardware IRQ `irq` (0-15) fires, and unmask it. */
void irq_install(int irq, irq_handler_t handler);

static inline void interrupts_enable(void)  { __asm__ volatile ("sti"); }
static inline void interrupts_disable(void) { __asm__ volatile ("cli"); }

/* Sleep the CPU until the next interrupt arrives (saves power, too). */
static inline void cpu_wait(void) { __asm__ volatile ("hlt"); }

#endif
