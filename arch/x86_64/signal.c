/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * signal.c — the x86_64 signal frame (kernel/signal.c has the rest)
 *
 * On x86_64 there is one kind of frame, Linux's rt_sigframe, for every
 * handler (with or without SA_SIGINFO). On the program's stack, from the
 * new stack pointer up:
 *
 *   pretcode    the C library's restorer (musl: __restore_rt, which does
 *               `mov $15, %eax; syscall` = rt_sigreturn). The handler's
 *               `ret` jumps there.
 *   ucontext    uc_flags, uc_link, uc_stack, then uc_mcontext — the saved
 *               registers (struct sigcontext, 256 bytes) — and uc_sigmask
 *   siginfo     128 bytes
 *   ...         the FPU/SSE registers (fxsave format), 64-byte aligned
 *   ...         128 bytes left alone: the interrupted code's "red zone"
 *               (the x86_64 ABI lets a function use the 128 bytes below its
 *               stack pointer without moving it, so we must not touch them)
 *
 * The handler is called as handler(sig, &info, &uc): RDI, RSI, RDX.
 */
#include <errno.h>
#include <fpu.h>
#include <gdt.h>
#include <interrupts.h>
#include <proc.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <thread.h>
#include <uaccess.h>
#include <user.h>

struct sigcontext {                 /* 256 bytes (asm/sigcontext.h) */
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;               /* user address of the FPU state */
    uint64_t reserved[8];
};

struct siginfo {                    /* 128 bytes */
    int32_t si_signo, si_errno, si_code, pad0;
    int32_t timer_id, overrun;      /* SI_TIMER: si_tid, si_overrun */
    uint64_t value;                 /*           si_value */
    int32_t pad[24];
};

struct ucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp;
    int32_t ss_flags, pad0;
    uint64_t ss_size;
    struct sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};

struct rt_sigframe {
    uint64_t pretcode;
    struct ucontext uc;
    struct siginfo info;
};

_Static_assert(offsetof(struct rt_sigframe, uc.uc_mcontext) == 48, "sigframe layout");
_Static_assert(offsetof(struct rt_sigframe, info) == 312, "sigframe layout");
_Static_assert(sizeof(struct siginfo) == SIGINFO_SIZE, "siginfo size");

#define RED_ZONE 128

/* RFLAGS bits a program may change through sigreturn: the arithmetic flags,
   direction, single-step, alignment check. Not IF or IOPL. */
#define FLAGS_USER 0x00040DD5

static void fill_info(struct siginfo *si, int sig, const struct sig_origin *o)
{
    memset(si, 0, sizeof *si);
    si->si_signo = sig;
    si->si_code = o->code;
    si->timer_id = o->timer_id;
    si->overrun = o->overrun;
    si->value = o->value;
}

void arch_siginfo(void *si, int sig, const struct sig_origin *o)
{
    fill_info(si, sig, o);
}

void arch_syscall_restart(struct regs *r, uint32_t nr)
{
    r->rax = nr;
    r->rip -= 2;                        /* syscall is 0F 05 */
}

void arch_syscall_result(struct regs *r, long value)
{
    r->rax = (uint64_t)value;
}

int arch_signal_frame(int sig, const struct sig_origin *o, const struct k_sigaction *a,
                      uint64_t mask, struct regs *r)
{
    struct thread *t = thread_current();
    /* Linux on x86_64 needs the C library's restorer; there is no default
       one in a kernel-provided page. And iretq to a non-user address would
       fault in the kernel, so the handler must be a user address. */
    if (!(a->flags & SA_RESTORER) || a->handler >= USER_ADDR_END)
        return -1;

    /* On the alternate stack if the handler asked for it (SA_ONSTACK); the
       red zone belongs to the interrupted code, so it's only skipped on
       the stack that code was using. */
    int alt;
    uint64_t sp = sig_frame_top(a, r->rsp, &alt);
    if (!alt)
        sp -= RED_ZONE;
    uint64_t fpstate = 0;
    if (t->uses_fpu) {
        sp = (sp - FPU_STATE_SIZE) & ~(uint64_t)63;
        fpu_save(t->fpu_state);
        if (copy_to_user((void *)sp, t->fpu_state, FPU_STATE_SIZE) < 0)
            return -1;
        fpstate = sp;
    }

    struct rt_sigframe f;
    memset(&f, 0, sizeof f);
    /* At the handler's first instruction RSP + 8 must be 16-byte aligned,
       as just after a `call`. */
    sp = ((sp - sizeof f) & ~(uint64_t)15) - 8;
    f.pretcode = a->restorer;

    struct sigcontext *sc = &f.uc.uc_mcontext;
    sc->r8 = r->r8;   sc->r9 = r->r9;   sc->r10 = r->r10; sc->r11 = r->r11;
    sc->r12 = r->r12; sc->r13 = r->r13; sc->r14 = r->r14; sc->r15 = r->r15;
    sc->rdi = r->rdi; sc->rsi = r->rsi; sc->rbp = r->rbp; sc->rbx = r->rbx;
    sc->rdx = r->rdx; sc->rax = r->rax; sc->rcx = r->rcx; sc->rsp = r->rsp;
    sc->rip = r->rip;
    sc->eflags = r->rflags;
    sc->cs = (uint16_t)r->cs;
    sc->ss = (uint16_t)r->ss;
    sc->err = r->err_code;
    sc->trapno = r->vector;
    sc->oldmask = mask;
    sc->fpstate = fpstate;
    f.uc.uc_sigmask = mask;
    user_addr_t ss_sp, ss_size;
    int32_t ss_flags;
    sig_save_altstack(r->rsp, &ss_sp, &ss_flags, &ss_size);
    f.uc.ss_sp = ss_sp;
    f.uc.ss_flags = ss_flags;
    f.uc.ss_size = ss_size;
    fill_info(&f.info, sig, o);
    if (copy_to_user((void *)sp, &f, sizeof f) < 0)
        return -1;

    r->rdi = (uint64_t)sig;
    r->rsi = sp + offsetof(struct rt_sigframe, info);
    r->rdx = sp + offsetof(struct rt_sigframe, uc);
    r->rax = 0;                         /* (no vector registers used: varargs) */
    r->rsp = sp;
    r->rip = a->handler;
    r->rflags &= ~(uint64_t)(0x100 | 0x400);    /* no single-step, direction up */
    if (t->uses_fpu) {
        fpu_clean_state(t->fpu_state);
        fpu_restore(t->fpu_state);
    }
    return 0;
}

/* rt_sigreturn: the handler's `ret` popped pretcode, so the frame starts 8
   bytes below the stack pointer the restorer's syscall came with. */
intptr_t sys_sigreturn_impl(int rt)
{
    (void)rt;                           /* x86_64 has only the rt kind */
    struct thread *t = thread_current();
    struct regs *r = t->uregs;
    uint64_t frame = r->rsp - 8;
    struct sigcontext sc;
    uint64_t mask;
    struct ucontext uc;
    if (copy_from_user(&uc, (void *)(frame + offsetof(struct rt_sigframe, uc)), sizeof uc) < 0)
        proc_exit(W_SIGNALED(SIGSEGV));
    sc = uc.uc_mcontext;
    mask = uc.uc_sigmask;
    /* The way back to user mode is iretq, which faults IN THE KERNEL on a
       return address that isn't canonical. The frame is the program's to
       write: such a frame is a broken program, not a kernel crash. */
    if (sc.rip >= USER_ADDR_END || sc.rsp >= USER_ADDR_END)
        proc_exit(W_SIGNALED(SIGSEGV));

    /* The registers back — but never the privilege level (CS, SS) or the
       system flags. */
    r->r8 = sc.r8;   r->r9 = sc.r9;   r->r10 = sc.r10; r->r11 = sc.r11;
    r->r12 = sc.r12; r->r13 = sc.r13; r->r14 = sc.r14; r->r15 = sc.r15;
    r->rdi = sc.rdi; r->rsi = sc.rsi; r->rbp = sc.rbp; r->rbx = sc.rbx;
    r->rdx = sc.rdx; r->rax = sc.rax; r->rcx = sc.rcx; r->rsp = sc.rsp;
    r->rip = sc.rip;
    r->rflags = (r->rflags & ~(uint64_t)FLAGS_USER) | (sc.eflags & FLAGS_USER);
    r->cs = GDT_USER_CODE;
    r->ss = GDT_USER_DATA;
    t->sig_blocked = mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    sig_restore_altstack(uc.ss_sp, uc.ss_flags, uc.ss_size);

    if (sc.fpstate && t->uses_fpu) {
        if (copy_from_user(t->fpu_state, (void *)sc.fpstate, FPU_STATE_SIZE) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));
        uint32_t *mxcsr = (uint32_t *)(t->fpu_state + 24);
        *mxcsr &= 0xFFBF;           /* reserved bits would make fxrstor fault */
        fpu_restore(t->fpu_state);
    }
    return (intptr_t)r->rax;        /* the syscall's result = the old RAX */
}
