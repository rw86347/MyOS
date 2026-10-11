/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ARCH_I386_PIC_H
#define ARCH_I386_PIC_H

#define PIC_IRQ_BASE 32     /* IRQ n arrives as CPU vector 32 + n */

void pic_init(void);        /* remap to vectors 32-47, mask all IRQs */
void pic_unmask(int irq);
void pic_mask(int irq);     /* (Linux's disable_irq) */
void pic_send_eoi(int irq);
int  pic_is_spurious(int irq);

#endif
