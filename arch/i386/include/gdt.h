#ifndef ARCH_I386_GDT_H
#define ARCH_I386_GDT_H

/* Segment selectors (see gdt.c for the table). */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x1B
#define GDT_USER_DATA   0x23

/* Install the kernel's GDT and reload all segment registers. */
void gdt_init(void);

#endif
