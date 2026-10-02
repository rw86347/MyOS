#ifndef ARCH_I386_GDT_H
#define ARCH_I386_GDT_H

/* Segment selectors (see gdt.c for the table). */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x1B
#define GDT_USER_DATA   0x23
#define GDT_TSS         0x28

#ifndef __ASSEMBLER__
#include <stdint.h>

/* Install the kernel's GDT and reload all segment registers. */
void gdt_init(void);

/* The stack the CPU switches to when a user program enters the kernel
   (syscall, interrupt, fault). Set on every thread switch. */
void tss_set_kernel_stack(uint32_t esp0);

/* Thread-local storage segments: GDT entries 6-8. */
#define GDT_TLS_FIRST   6
#define GDT_TLS_ENTRIES 3
void     gdt_load_tls(const uint64_t tls[GDT_TLS_ENTRIES]);
uint64_t gdt_tls_descriptor(uint32_t base, uint32_t limit, uint32_t flags);
#endif

#endif
