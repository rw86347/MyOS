/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * sysenter.c — set up fast system calls (see sysentry.asm for how they work)
 */
#include <cpu.h>
#include <gdt.h>
#include <interrupts.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <sched.h>
#include <proc.h>
#include <signal.h>
#include <string.h>
#include <syscall.h>
#include <thread.h>
#include <sysenter.h>
#include <uaccess.h>

#define MSR_SYSENTER_CS  0x174      /* kernel code segment (SS = CS + 8) */
#define MSR_SYSENTER_ESP 0x175      /* stack to start on */
#define MSR_SYSENTER_EIP 0x176      /* where to jump */

void sysenter_entry(void);
extern const char vsyscall_code[], vsyscall_code_end[];

/* sysenter loads ESP from an MSR, but each thread has its own kernel
   stack. Rewriting the MSR on every thread switch would cost ~100 cycles,
   so the MSR points at this little spare stack instead, and the first
   instruction of sysenter_entry switches to the real one (from the TSS). */
static uint8_t spare_stack[64] __attribute__((aligned(16)));

static uint32_t vsyscall_frame;     /* the vsyscall page, shared by all */

void sysenter_init(void)
{
    if (!cpu_has(CPU_SEP))
        return;                     /* programs will use int 0x80 */

    wrmsr(MSR_SYSENTER_CS, GDT_KERNEL_CODE);
    wrmsr(MSR_SYSENTER_ESP, (uint32_t)(spare_stack + sizeof spare_stack));
    wrmsr(MSR_SYSENTER_EIP, (uint32_t)sysenter_entry);

    vsyscall_frame = pmm_alloc();
    memset(P2V(vsyscall_frame), 0, PAGE_SIZE);
    memcpy(P2V(vsyscall_frame), vsyscall_code, vsyscall_code_end - vsyscall_code);
}

uint32_t vsyscall_map(void)
{
    if (!vsyscall_frame)
        return 0;
    /* Read-only and shared: one physical page for every program, which
       freeing an address space must leave alone (PTE_SHARED). */
    if (vmm_map(VSYSCALL_ADDR, vsyscall_frame, PTE_USER | PTE_SHARED) < 0)
        return 0;
    return VSYSCALL_ADDR;
}

/* Called from sysenter_entry with the same frame int 0x80 builds. */
void sysenter_dispatch(struct regs *r)
{
    /* Argument 6 would be in EBP, but EBP carries the user stack pointer;
       the vsyscall code pushed the real EBP on the user stack. */
    uint32_t a6 = 0;
    get_user32(&a6, r->user_esp);
    thread_current()->uregs = r;        /* fork/execve need the registers */
    r->eax = syscall_dispatch(r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, a6);
    sched_preempt_check();
    SIGNAL_CHECK(r);                    /* back to user mode: deliver signals */
}
