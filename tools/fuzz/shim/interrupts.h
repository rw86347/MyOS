#ifndef ARCH_I386_INTERRUPTS_H
#define ARCH_I386_INTERRUPTS_H

/*
 * Host stand-in for arch/i386/include/interrupts.h, so the net sources compile for
 * the fuzzer. The real one is full of i386 inline assembly (cli/sti/pushfl).
 *
 * The fuzzer is single-threaded and nothing here runs from an interrupt, so
 * irq_save/irq_restore can be nothing at all. That is also the one thing the
 * fuzzer cannot test: a bug that needs an interrupt to land between two
 * instructions is invisible here. Races stay a reading problem.
 */

#include <stdint.h>

struct regs {
    uint32_t vector, err_code;
    uint32_t eip, cs, eflags;
    uint32_t user_esp, user_ss;
};

typedef void (*irq_handler_t)(struct regs *r);

static inline void     interrupts_enable(void)  { }
static inline void     interrupts_disable(void) { }
static inline uint32_t irq_save(void)           { return 0; }
static inline void     irq_restore(uint32_t f)  { (void)f; }
static inline void     cpu_wait(void)           { }

#endif
