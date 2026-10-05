#ifndef ARCH_X86_64_INTERRUPTS_H
#define ARCH_X86_64_INTERRUPTS_H

#include <stdint.h>

/*
 * Saved register frame for x86_64 (matches isr.asm push order, low address first).
 * Syscall/sysenter on i386 used a different layout; here musl uses the syscall
 * instruction and this is the kernel-side view.
 */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, err_code;
    uint64_t rip, cs, rflags;
    uint64_t rsp, ss;
};

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void irq_install(int irq, irq_handler_t handler);

static inline void interrupts_enable(void)  { __asm__ volatile ("sti"); }
static inline void interrupts_disable(void) { __asm__ volatile ("cli"); }

static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & (1ull << 9))
        __asm__ volatile ("sti" : : : "memory");
}

static inline void cpu_wait(void) { __asm__ volatile ("hlt"); }

/* Portable kernel code uses these names on both architectures. */
static inline void regs_set_ip(struct regs *r, uint64_t ip) { r->rip = ip; }
static inline uint64_t regs_ip(const struct regs *r) { return r->rip; }
static inline void regs_set_sp(struct regs *r, uint64_t sp) { r->rsp = sp; }
static inline uint64_t regs_sp(const struct regs *r) { return r->rsp; }
static inline void regs_set_ax(struct regs *r, int32_t v) { r->rax = (uint64_t)(int64_t)v; }
static inline int regs_user_mode(const struct regs *r) { return (r->cs & 3) == 3; }

#endif
