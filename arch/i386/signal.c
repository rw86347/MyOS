/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * signal.c — the i386 signal frame (kernel/signal.c has the rest)
 *
 * When a handler runs, the program's registers, signal mask and FPU state
 * are saved in a frame on its own stack, laid out exactly as Linux does —
 * the C library's restorer and anything reading the ucontext depend on it.
 * The handler returns into the restorer, which calls sigreturn; that reads
 * the frame back.
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

struct sigcontext {                 /* 88 bytes */
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip, cs, eflags, esp_at_signal, ss;
    uint32_t fpstate;               /* user address of the FPU state */
    uint32_t oldmask, cr2;
};

struct siginfo {                    /* 128 bytes */
    int32_t si_signo, si_errno, si_code;
    int32_t timer_id, overrun;      /* SI_TIMER: si_tid, si_overrun */
    uint32_t value;                 /*           si_value */
    int32_t pad[26];
};

struct ucontext {
    uint32_t uc_flags, uc_link;
    uint32_t ss_sp, ss_flags, ss_size;
    struct sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};

struct sigframe {                   /* without SA_SIGINFO: handler(sig) */
    uint32_t pretcode;
    int32_t sig;
    struct sigcontext sc;
    uint32_t extramask;             /* the high half of the blocked mask */
};

struct rt_sigframe {                /* with SA_SIGINFO: handler(sig, info, uc) */
    uint32_t pretcode;
    int32_t sig;
    uint32_t pinfo, puc;
    struct siginfo info;
    struct ucontext uc;
};

/* EFLAGS bits a program may change through sigreturn (the rest — IOPL,
   VM, interrupt flag... — must stay as the kernel says). */
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

static void save_context(struct sigcontext *sc, const struct regs *r, uint64_t mask,
                         uint32_t fpstate)
{
    sc->gs = r->gs;   sc->fs = r->fs;   sc->es = r->es;   sc->ds = r->ds;
    sc->edi = r->edi; sc->esi = r->esi; sc->ebp = r->ebp; sc->esp = r->user_esp;
    sc->ebx = r->ebx; sc->edx = r->edx; sc->ecx = r->ecx; sc->eax = r->eax;
    sc->trapno = r->vector;
    sc->err = r->err_code;
    sc->eip = r->eip;
    sc->cs = r->cs;
    sc->eflags = r->eflags;
    sc->esp_at_signal = r->user_esp;
    sc->ss = r->user_ss;
    sc->fpstate = fpstate;
    sc->oldmask = (uint32_t)mask;
    sc->cr2 = 0;
}

int arch_signal_frame(int sig, const struct sig_origin *o, const struct k_sigaction *a,
                      uint64_t mask, struct regs *r)
{
    struct thread *t = thread_current();

    /* The FPU registers: save them below the frame (16-byte aligned for
       fxsave), and give the handler a clean FPU. All of it on the
       alternate stack if the handler asked for it (SA_ONSTACK). */
    int alt;
    uint32_t sp = ((uint32_t)sig_frame_top(a, r->user_esp, &alt) - FPU_STATE_SIZE) & ~0xFu;
    user_addr_t ss_sp, ss_size;
    int32_t ss_flags;
    uint32_t fpstate = 0;
    if (t->uses_fpu) {
        fpu_save(t->fpu_state);
        if (copy_to_user((void *)sp, t->fpu_state, FPU_STATE_SIZE) < 0)
            return -1;
        fpstate = sp;
    }

    if (a->flags & SA_SIGINFO) {
        struct rt_sigframe f;
        memset(&f, 0, sizeof f);
        /* At the handler's first instruction, (esp + 4) must be 16-byte
           aligned, as after a normal `call`. */
        sp = ((sp - sizeof f + 4) & ~0xFu) - 4;
        f.pretcode = a->restorer;
        f.sig = sig;
        f.pinfo = sp + offsetof(struct rt_sigframe, info);
        f.puc = sp + offsetof(struct rt_sigframe, uc);
        fill_info(&f.info, sig, o);
        save_context(&f.uc.uc_mcontext, r, mask, fpstate);
        f.uc.uc_sigmask = mask;
        sig_save_altstack(r->user_esp, &ss_sp, &ss_flags, &ss_size);
        f.uc.ss_sp = ss_sp;
        f.uc.ss_flags = (uint32_t)ss_flags;
        f.uc.ss_size = ss_size;
        if (copy_to_user((void *)sp, &f, sizeof f) < 0)
            return -1;
        r->edx = f.pinfo;               /* (also in registers, as Linux does) */
        r->ecx = f.puc;
    } else {
        struct sigframe f;
        memset(&f, 0, sizeof f);
        sp = ((sp - sizeof f + 4) & ~0xFu) - 4;
        f.pretcode = a->restorer;
        f.sig = sig;
        save_context(&f.sc, r, mask, fpstate);
        f.extramask = (uint32_t)(mask >> 32);
        if (copy_to_user((void *)sp, &f, sizeof f) < 0)
            return -1;
        r->edx = r->ecx = 0;
        sig_save_altstack(r->user_esp, &ss_sp, &ss_flags, &ss_size);  /* (disarms only) */
    }

    r->user_esp = sp;
    r->eip = a->handler;
    r->eax = sig;
    r->eflags &= ~(0x100 | 0x400);      /* no single-step, direction up */
    if (t->uses_fpu) {
        fpu_clean_state(t->fpu_state);
        fpu_restore(t->fpu_state);
    }
    return 0;
}

intptr_t sys_sigreturn_impl(int rt)
{
    struct thread *t = thread_current();
    struct regs *r = t->uregs;
    struct sigcontext sc;
    uint64_t mask;
    struct ucontext uc;

    /* The handler's `ret` popped pretcode; the restorer may have popped
       sig too (non-rt). That tells us where the frame starts. */
    if (rt) {
        uint32_t frame = r->user_esp - 4;
        if (copy_from_user(&uc, (void *)(frame + offsetof(struct rt_sigframe, uc)), sizeof uc) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));
        sc = uc.uc_mcontext;
        mask = uc.uc_sigmask;
    } else {
        uint32_t frame = r->user_esp - 8, high;
        if (copy_from_user(&sc, (void *)(frame + offsetof(struct sigframe, sc)), sizeof sc) < 0 ||
            copy_from_user(&high, (void *)(frame + offsetof(struct sigframe, extramask)), 4) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));
        mask = ((uint64_t)high << 32) | sc.oldmask;
    }

    /* Put the registers back — but never let a program choose its own
       privilege level (segments) or system flags. */
    r->edi = sc.edi; r->esi = sc.esi; r->ebp = sc.ebp; r->ebx = sc.ebx;
    r->edx = sc.edx; r->ecx = sc.ecx; r->eax = sc.eax;
    r->eip = sc.eip;
    r->user_esp = sc.esp;
    r->eflags = (r->eflags & ~FLAGS_USER) | (sc.eflags & FLAGS_USER);
    r->cs = GDT_USER_CODE;
    r->user_ss = GDT_USER_DATA;
    /* TLS segments. The ring-3 bits are not enough on their own: the frame is
       the program's to write, and a selector that cannot be loaded faults in
       the kernel's own return path, which is fatal. */
    r->gs = gdt_user_seg_ok(sc.gs) ? sc.gs : GDT_USER_DATA;
    r->fs = gdt_user_seg_ok(sc.fs) ? sc.fs : GDT_USER_DATA;
    r->ds = r->es = GDT_USER_DATA;
    t->sig_blocked = mask & ~(SIGBIT(SIGKILL) | SIGBIT(SIGSTOP));
    if (rt)
        sig_restore_altstack(uc.ss_sp, (int32_t)uc.ss_flags, uc.ss_size);

    if (sc.fpstate && t->uses_fpu) {
        if (copy_from_user(t->fpu_state, (void *)sc.fpstate, FPU_STATE_SIZE) < 0)
            proc_exit(W_SIGNALED(SIGSEGV));
        uint32_t *mxcsr = (uint32_t *)(t->fpu_state + 24);
        *mxcsr &= 0xFFBF;           /* reserved bits would make fxrstor fault */
        fpu_restore(t->fpu_state);
    }
    return (int32_t)r->eax;         /* the syscall's result = the old EAX */
}
