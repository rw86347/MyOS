/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * syscall.c — EL0 SVC dispatch (step 14a/14b bridge)
 */
#include "syscall.h"

#include <kheap.h>
#include <kprintf.h>
#include <memlayout.h>
#include <pmm.h>

#include "elf64_load.h"
#include "uaccess.h"
#include "uart.h"

static volatile int s_el0_pass;
static void *s_el1_resume_pc;

void el0_set_resume(void *el1_pc)
{
    s_el1_resume_pc = el1_pc;
}

extern void arm64_return_to_el1(void *el1_pc);

int el0_test_passed(void)
{
    return s_el0_pass;
}

static uint64_t sysret_err(int err)
{
    return (uint64_t)(-(long)err);
}

static uint64_t sys_write(struct el0_regs *regs)
{
    int fd = (int)regs->x[0];
    const void *buf = (const void *)(uintptr_t)regs->x[1];
    size_t count = (size_t)regs->x[2];
    char tmp[128];
    size_t chunk;
    size_t total = 0;

    if (fd != 1 && fd != 2)
        return sysret_err(9); /* EBADF */
    while (total < count) {
        chunk = count - total;
        if (chunk > sizeof tmp)
            chunk = sizeof tmp;
        if (copy_from_user(tmp, (const char *)buf + total, chunk) < 0)
            return sysret_err(14); /* EFAULT */
        if (fd == 1 || fd == 2) {
            for (size_t i = 0; i < chunk; i++) {
                char c = tmp[i];

                if (c == '\n')
                    uart_putc('\r');
                uart_putc(c);
            }
        }
        total += chunk;
    }
    s_el0_pass = 1;
    return total;
}

uint64_t syscall_dispatch_el0(uint64_t nr, struct el0_regs *regs)
{
    switch (nr) {
    case __NR_write:
        return sys_write(regs);
    case __NR_exit:
    case __NR_exit_group:
        s_el0_pass = 1;
        for (;;)
            __asm__ volatile("wfe");
    case SYS_myos_el0_ping:
        if (regs->x[0] == 0x44415441ULL) {
            kprintf("  EL0: data access ok\n");
            s_el0_pass = 1;
            return 0;
        }
        kprintf("  EL0 svc: ping arg0=%llu\n", (unsigned long long)regs->x[0]);
        kprintf("  EL0: svc probe ok\n");
        s_el0_pass = 1;
        return 0;
    case SYS_myos_el0_done:
        if (s_el1_resume_pc)
            arm64_return_to_el1(s_el1_resume_pc);
        return 0;
    default:
        kprintf("  EL0 svc: unknown nr %llu\n", (unsigned long long)nr);
        return (uint64_t)-1;
    }
}

void sync_exception_el0(uint64_t esr, uint64_t elr, struct el0_regs *regs)
{
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);

    (void)elr;
    if (ec == ESR_EC_SVC64) {
        uint64_t nr = regs->x[8];

        regs->x[0] = syscall_dispatch_el0(nr, regs);
        return;
    }
    uart_puts("\n*** sync exception EL0 *** ESR=0x");
    uart_puthex((unsigned long)esr);
    uart_puts(" ELR=0x");
    uart_puthex((unsigned long)elr);
    uart_puts("\n");
    for (;;)
        __asm__ volatile("wfe");
}

extern void user_el0_stub(void);
extern void user_write_test(void);
extern void user_el0_databtest(void);
extern void arm64_enter_el0(void *entry, void *sp_el0);

static void el0_start_user(void *entry);

static void *s_user_entry;
static void *s_user_sp;

void el0_run_probe(void)
{
    el0_start_user((void *)user_el0_stub);
}

void el0_run_write_test(void)
{
    el0_start_user((void *)user_write_test);
}

void el0_run_databtest(void)
{
    el0_start_user((void *)user_el0_databtest);
}

extern char musl_hello_blob[];
extern char musl_hello_blob_end[];

void el0_run_musl_hello(void)
{
    struct arm64_elf_loaded loaded;
    size_t len = (size_t)(musl_hello_blob_end - musl_hello_blob);

    if (arm64_elf_load_exec(musl_hello_blob, len, &loaded) < 0) {
        kprintf("  EL0: musl hello ELF load failed\n");
        return;
    }
    s_user_entry = loaded.entry;
    s_user_sp = loaded.user_sp;
    kprintf("  EL0: musl hello entry=%p sp=%p (%u bytes)\n", s_user_entry, s_user_sp,
            (unsigned)len);
}

static void el0_start_user(void *entry)
{
    uint32_t frame = pmm_alloc();

    if (!frame) {
        uart_puts("  EL0: no stack page\n");
        return;
    }
    uint8_t *stack_page = P2V(frame);

    s_user_sp = stack_page + 4096 - 16;
    s_user_entry = entry;
    kprintf("  EL0: entering user at %p sp=%p\n", s_user_entry, s_user_sp);
}

void el0_enter_user(void)
{
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb sy");
    arm64_enter_el0(s_user_entry, s_user_sp);
}
