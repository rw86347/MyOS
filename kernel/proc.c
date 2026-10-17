/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * proc.c — processes: start, fork, execve, exit, wait
 *
 * Speed (the step-8 rules in CLAUDE.md):
 *   fork    copies no memory: parent and child share every page
 *           copy-on-write (paging.c); only the page tables are copied.
 *   execve  reads the program straight from the initramfs; its stack and
 *           heap pages appear only when touched (demand paging, mm.c).
 *   exit    frees everything at once; the parent is woken, not polling.
 */
#include <errno.h>
#include <futex.h>
#include <gdt.h>
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <ipc.h>
#include <mm.h>
#include <proc.h>
#include <string.h>
#include <thread.h>
#include <timer.h>
#include <uaccess.h>
#include <usermode.h>
#include <vfs.h>

static struct process procs[PROC_MAX];
static int next_pid = 1;
static struct process *init_proc;
static struct wait_queue kernel_wait;   /* the kernel waiting for a program */

struct process *proc_current(void)
{
    return thread_current()->proc;
}

struct process *proc_at(int i)
{
    return &procs[i];
}

struct process *proc_find(int pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state != PROC_FREE && procs[i].pid == pid)
            return &procs[i];
    return NULL;
}

int proc_next_pid(void) { return next_pid; }

void proc_set_init(struct process *p)
{
    init_proc = p;
}

/* A free process slot with a fresh pid, or NULL. Interrupts off. */
static struct process *proc_alloc(void)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = &procs[i];
        if (p->state == PROC_FREE) {
            memset(p, 0, sizeof *p);
            fd_table_init(p);
            p->pid = next_pid++;
            p->state = PROC_ALIVE;
            p->cwd = vfs_root();
            vfs_get(p->cwd);
            p->umask = 022;
            p->pgid = p->sid = p->pid;  /* its own group and session */
            p->start_ms = timer_ms();
            procfs_pid_add(p->pid);     /* /proc/<pid> */
            return p;
        }
    }
    return NULL;
}

static void proc_clear_exe(struct process *p)
{
    if (p->exe) {
        vfs_put(p->exe);
        p->exe = NULL;
    }
}

static void reap(struct process *p)
{
    if (p->cwd) {                       /* a process that never ran */
        vfs_put(p->cwd);
        p->cwd = NULL;
    }
    if (p->root) {
        vfs_put(p->root);
        p->root = NULL;
    }
    proc_clear_exe(p);
    procfs_pid_remove(p->pid);
    p->state = PROC_FREE;
}

/* A process's current directory is in use: if it's deleted, it stays
   around (empty) until the process moves on. */
void proc_set_cwd(struct process *p, struct inode *dir)
{
    vfs_get(dir);
    if (p->cwd)
        vfs_put(p->cwd);
    p->cwd = dir;
}

/* t becomes one of p's threads, with thread id tid. Interrupts off, and
   before t first runs (it must know its tid). */
static void thread_join_proc(struct process *p, struct thread *t, int tid)
{
    t->tid = tid;
    t->proc_next = p->threads;
    p->threads = t;
    p->nthreads++;
}

static void thread_leave_proc(struct process *p, struct thread *t)
{
    struct thread **pp = &p->threads;
    while (*pp && *pp != t)
        pp = &(*pp)->proc_next;
    if (*pp)
        *pp = t->proc_next;
    t->proc_next = NULL;
    p->nthreads--;
    p->utime += t->utime;               /* its CPU time stays the process's */
    p->stime += t->stime;
    ptimer_thread_gone(p, t);
}

/* The name a program's thread gets: "/bin/hello" -> "hello". */
static const char *base_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            name = p + 1;
    return name;
}

/* --- starting programs ------------------------------------------------------ */

/* A new program's thread starts here, already in its address space. */
static void start_user(void *arg)
{
    struct process *p = arg;
    thread_start_fpu();                 /* programs may use floats / SSE */
#ifdef ARCH_X86_64
    user_enter((uint64_t)p->start_eip, (uint64_t)p->start_esp);
#else
    user_enter(p->start_eip, p->start_esp);
#endif
}

struct process *proc_spawn(const char *path, const char *const argv[],
                           const char *const envp[])
{
    uint32_t flags = irq_save();
    struct process *p = proc_alloc();
    irq_restore(flags);
    if (!p)
        return NULL;

    p->mm = mm_create();
    if (!p->mm) {
        reap(p);
        return NULL;
    }
    /* Load the program into its new address space: we step into it for
       a moment (we're a kernel thread, so we can borrow any). */
    struct mm *old = mm_current();
    mm_switch(p->mm);
    int err = program_load(path, argv, envp, &p->start_eip, &p->start_esp,
                           &p->cred, &p->persona, &p->exe);
    mm_switch(old);
    if (err) {
        mm_destroy(p->mm);
        reap(p);
        return NULL;
    }

    fd_open_console(p);                 /* stdin, stdout, stderr */
    p->kernel_waits = 1;
    flags = irq_save();
    struct thread *t = thread_create_in(p, base_name(path), start_user, p);
    if (t)
        thread_join_proc(p, t, p->pid);
    irq_restore(flags);
    if (!t) {
        fd_close_all(p);
        mm_destroy(p->mm);
        reap(p);
        return NULL;
    }
    return p;
}

int proc_wait_kernel(struct process *p)
{
    uint32_t flags = irq_save();
    while (p->state != PROC_ZOMBIE)
        wq_sleep(&kernel_wait);
    int status = p->status;
    reap(p);
    irq_restore(flags);
    return status;
}

/* Is pgid a live process group in session sid? TIOCSPGRP has to know, because
   the foreground group is who gets the keyboard and SIGINT: handing the
   terminal to a group in someone else's session lets any process take the
   keyboard away from the shell that owns it. A scan of the table, which is
   fine for an ioctl a shell calls once per job. */
int proc_pgid_in_session(int pgid, int sid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_at(i);
        if (p->state == PROC_ALIVE && p->pgid == pgid && p->sid == sid)
            return 1;
    }
    return 0;
}

/* --- fork ----------------------------------------------------------------- */

int proc_fork(void)
{
    return proc_fork_frame(thread_current()->uregs, NULL);
}

#define CLONE_VM             0x00000100
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* CLONE_SETTLS: on i386 the argument is a user_desc, as for
   set_thread_area; on x86_64 it is the new thread's FS base itself (see
   sys_arch_prctl). Returns the TLS slot (0..GDT_TLS_ENTRIES-1) and the
   value for thread->tls[slot], or -1. */
static int read_tls(uintptr_t tls, uint64_t *desc)
{
#ifdef ARCH_X86_64
    if (tls >= USER_ADDR_END)
        return -1;                      /* (a non-canonical one would fault in wrmsr) */
    *desc = tls;
    return 0;
#else
    uint32_t d[4];
    if (copy_from_user(d, (void *)tls, sizeof d) < 0 ||
        d[0] < GDT_TLS_FIRST || d[0] >= GDT_TLS_FIRST + GDT_TLS_ENTRIES)
        return -1;
    *desc = gdt_tls_descriptor(d[1], d[2], d[3]);
    return d[0] - GDT_TLS_FIRST;
#endif
}

/* A new thread in this process (pthread_create). It starts by "returning"
   from clone with EAX = 0 on the stack it was given, where musl has put
   the function to run. */
static int clone_thread(uint32_t flags, uintptr_t new_sp, uintptr_t parent_tid,
                        uintptr_t tls, uintptr_t child_tid)
{
    if ((flags & (CLONE_VM | CLONE_SIGHAND)) != (CLONE_VM | CLONE_SIGHAND))
        return -EINVAL;                 /* Linux insists on these too */
    struct process *p = proc_current();
    struct thread *me = thread_current();
    uint64_t desc = 0;
    int slot = -1;
    if ((flags & CLONE_SETTLS) && (slot = read_tls(tls, &desc)) < 0)
        return -EINVAL;

    struct regs frame = *me->uregs;
    if (new_sp)
#ifdef ARCH_X86_64
        frame.rsp = new_sp;
#else
        frame.user_esp = new_sp;
#endif
    uint32_t irq = irq_save();
    if (p->exiting) {
        irq_restore(irq);
        return -EAGAIN;
    }
    struct thread *t = thread_fork(p, me->name, &frame);
    if (!t) {
        irq_restore(irq);
        return -EAGAIN;
    }
    int tid = next_pid++;               /* tids and pids share one space */
    thread_join_proc(p, t, tid);
    if (slot >= 0)
        t->tls[slot] = desc;
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_tid = child_tid;
    t->ss_sp = t->ss_size = 0;          /* a thread starts without the */
    t->ss_flags = 0;                    /* creator's alternate stack (Linux) */
    t->sig_check = p->sig_pending != 0; /* (blocked ones it may take) */
    irq_restore(irq);
    /* Same address space, so writing the tid here is the same as the child
       writing it first thing — but only nearly: interrupts being off does
       not stop the new thread running, because the copies below can sleep
       (see fault_in_user in uaccess.h). Both words are written before we
       return either way, and musl does not read them before clone returns,
       so the order does not matter here. */
    if ((flags & CLONE_PARENT_SETTID) && parent_tid)
        copy_to_user((void *)parent_tid, &tid, sizeof tid);
    if ((flags & CLONE_CHILD_SETTID) && child_tid)
        copy_to_user((void *)child_tid, &tid, sizeof tid);
    return tid;
}

int proc_clone(uint32_t flags, uintptr_t new_sp, uintptr_t parent_tid, uintptr_t tls,
               uintptr_t child_tid)
{
    if (flags & CLONE_THREAD)
        return clone_thread(flags, new_sp, parent_tid, tls, child_tid);
    struct regs frame = *thread_current()->uregs;
    if (new_sp)
#ifdef ARCH_X86_64
        frame.rsp = new_sp;
#else
        frame.user_esp = new_sp;
#endif        /* the child runs on its own stack */
    struct process *child;
    int pid = proc_fork_frame(&frame, &child);
    if (pid < 0)
        return pid;
    uint64_t desc;
    int slot;
    if ((flags & CLONE_SETTLS) && (slot = read_tls(tls, &desc)) >= 0)
        child->threads->tls[slot] = desc;
    if ((flags & CLONE_CHILD_CLEARTID))
        child->threads->clear_tid = child_tid;
    if ((flags & CLONE_PARENT_SETTID) && parent_tid)
        copy_to_user((void *)parent_tid, &pid, sizeof pid);
    return pid;
}

int proc_fork_frame(const struct regs *frame, struct process **child_out)
{
    struct process *parent = proc_current();
    struct thread *me = thread_current();

    struct process *child = proc_alloc();
    if (!child)
        return -EAGAIN;
    child->mm = mm_fork(parent->mm);    /* page tables only: copy-on-write */
    if (!child->mm) {
        reap(child);
        return -ENOMEM;
    }
    child->parent = parent;
    proc_set_cwd(child, parent->cwd);
    if (parent->root) {                 /* a chroot is inherited */
        vfs_get(parent->root);
        child->root = parent->root;
    }
    child->umask = parent->umask;
    child->persona = parent->persona;
    child->cred = parent->cred;         /* same users and groups */
    child->pgid = parent->pgid;         /* same job, same session */
    child->sid = parent->sid;
    memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
    child->cmdline_len = parent->cmdline_len;
    signal_fork(child, parent);
    fd_copy_all(child, parent);         /* same open files, shared, same close-on-exec */
    if (parent->exe) {
        vfs_get(parent->exe);
        child->exe = parent->exe;
    }

    /* The child's thread starts by "returning" from this very syscall,
       with the parent's registers — except EAX = 0. Only the calling
       thread is copied: in the child, the parent's other threads are gone
       (as on Linux). */
    uint32_t irq = irq_save();
    struct thread *t = thread_fork(child, me->name, frame);
    if (t)
        thread_join_proc(child, t, child->pid);
    irq_restore(irq);
    if (!t) {
        fd_close_all(child);
        mm_destroy(child->mm);
        reap(child);
        return -EAGAIN;
    }
    if (child_out)
        *child_out = child;
    return child->pid;
}

/* --- execve ------------------------------------------------------------------ */

#define EXEC_MAX_ARGS 1024              /* argv + envp entries */
#define EXEC_STR_SPACE (64 * 1024)      /* all their strings together */

/* The arguments, copied out of the old program before it's thrown away. */
struct exec_args {
    char *argv[EXEC_MAX_ARGS + 1];      /* argv..., NULL, envp..., NULL */
    char **envp;
    uint32_t used;                      /* bytes of str[] in use */
    char str[EXEC_STR_SPACE];
};

/* Copy a NULL-terminated user array of strings into a. Returns the number
   copied, or -errno. */
/*
 * Copy one NULL-terminated vector of user strings into `a`, writing the
 * pointers to `out` and the bytes to a->str. Returns how many, not counting
 * the NULL it writes at out[n] -- so `out` must have room + 1 slots.
 *
 * `room < 0` is refused rather than trusted. The caller computes envp's room
 * by subtraction, and a negative one would make the `n == room` test below
 * unreachable (n is never negative) and the loop unbounded.
 */
static int copy_strings(struct exec_args *a, char **out, int room, uintptr_t user_vec)
{
    if (room < 0)
        return -E2BIG;
    if (!user_vec) {
        /* A NULL argv or envp is legal and means an empty vector -- but it
           still has to be terminated. Returning without writing the NULL left
           out[0] holding whatever kmalloc last had there, and count() in
           kernel/usermode.c walks that as a char * until it happens to find a
           zero word, then memcpy's each stale word onto the new program's
           stack: kernel heap handed to user space as argv, or a fault in
           kernel mode with no fixup. */
        out[0] = NULL;
        return 0;
    }
    for (int n = 0; ; n++) {
        user_addr_t ptr;                /* a pointer of the program's size */
        if (get_user_word(&ptr, user_vec + n * sizeof ptr) < 0)
            return -EFAULT;
        if (!ptr) {
            out[n] = NULL;
            return n;
        }
        if (n == room)
            return -E2BIG;
        long len = strncpy_from_user(a->str + a->used, (const char *)(uintptr_t)ptr,
                                     EXEC_STR_SPACE - a->used);
        if (len == -ENAMETOOLONG)
            return -E2BIG;
        if (len < 0)
            return len;
        out[n] = a->str + a->used;
        a->used += len + 1;
    }
}

#define EXITING_GROUP 1                 /* exit_group: the process ends */
#define EXITING_EXEC  2                 /* execve: all threads but one end */

/* Kill every other thread of p and wait until they're gone (execve). They
   die on their way back to user mode, like on a SIGKILL. Returns 0 if an
   exit_group got there first (then the whole process must end). */
static int end_other_threads(struct process *p)
{
    struct thread *me = thread_current();
    uint32_t irq = irq_save();
    if (p->exiting) {
        irq_restore(irq);
        return 0;
    }
    p->exiting = EXITING_EXEC;
    for (struct thread *t = p->threads; t; t = t->proc_next)
        if (t != me) {
            t->sig_pending |= SIGBIT(SIGKILL);
            t->sig_check = 1;
            thread_wake(t);
        }
    while (p->nthreads > 1)
        wq_sleep(&p->thread_gone);
    p->exiting = 0;
    irq_restore(irq);
    return 1;
}

int proc_execve(uintptr_t user_path, uintptr_t user_argv, uintptr_t user_envp)
{
    struct process *p = proc_current();
    struct exec_args *a = kmalloc(sizeof *a);
    if (!a)
        return -ENOMEM;
    a->used = 0;

    /* 1. Copy path, argv and envp out of the old program. */
    char path[PATH_MAX];
    long err = strncpy_from_user(path, (const char *)user_path, PATH_MAX);
    if (err == 0)
        err = -ENOENT;
    /*
     * EXEC_MAX_ARGS - 1, not EXEC_MAX_ARGS. argv and envp share one array and
     * each needs a NULL after it, so argc + envc + 2 pointers must fit in
     * EXEC_MAX_ARGS + 1 slots -- which bounds argc at EXEC_MAX_ARGS - 1.
     *
     * Letting argc reach EXEC_MAX_ARGS put envp at &a->argv[EXEC_MAX_ARGS + 1],
     * one past the end of the array, which is the address of the envp field
     * itself; and its room came out as -1, which no `n == room` can match. The
     * envp loop then ran with no bound, writing a pointer over `envp`, then
     * over `used`, and then copying user strings to wherever the corrupted
     * `used` pointed -- a kernel-mode store with no exception-table entry.
     * Reachable with 1024 argv entries and two envp entries.
     */
    int argc = err < 0 ? (int)err : copy_strings(a, a->argv, EXEC_MAX_ARGS - 1, user_argv);
    int envc = argc < 0 ? argc
             : copy_strings(a, a->envp = &a->argv[argc + 1],
                            EXEC_MAX_ARGS - argc - 1, user_envp);
    if (argc < 0 || envc < 0) {
        kfree(a);
        return argc < 0 ? argc : envc;
    }

    const char *prog = path;
    const char *const *argv = (const char *const *)a->argv;

    /* 2. Load it into a NEW address space (program_load also handles #!
       scripts). Until that succeeds, the old
       program is untouched — execve can still fail and return. */
    struct mm *new_mm = mm_create();
    if (!new_mm) {
        kfree(a);
        return -ENOMEM;
    }
    struct mm *old_mm = p->mm;
    mm_switch(new_mm);
    user_addr_t entry, sp;
    struct cred cred = p->cred;
    uint32_t persona = p->persona;
    struct inode *exe = NULL;
    err = program_load(prog, argv, (const char *const *)a->envp, &entry, &sp,
                       &cred, &persona, &exe);
    if (err) {
        mm_switch(old_mm);
        mm_destroy(new_mm);
        kfree(a);
        return err;
    }

    /* 3. Only the calling thread lives on in the new program. */
    if (p->nthreads > 1 && !end_other_threads(p)) {
        if (exe)
            vfs_put(exe);
        mm_switch(old_mm);              /* exit_group won the race: we die */
        mm_destroy(new_mm);
        kfree(a);
        proc_exit(p->status);
    }
    struct thread *me = thread_current();
    me->tid = p->pid;                   /* it now is the main thread */
    me->clear_tid = 0;
    me->ss_sp = me->ss_size = 0;        /* the old program's signal stack */
    me->ss_flags = 0;

    /* 4. Point of no return: the old program is gone. */
    p->mm = new_mm;
    p->cred = cred;
    p->persona = persona;
    mm_destroy(old_mm);
    for (int fd = 0; fd < (int)p->nfiles; fd++)
        if (fd_get_cloexec(p, fd))
            fd_close(fd);               /* close-on-exec files (clears the bit) */
    signal_exec(p);                     /* handlers were in the old program */
    proc_clear_exe(p);
    p->exe = exe;                       /* reference from program_load */
    thread_set_name(thread_current(), base_name(prog));
    /* Its command line, as /proc/<pid>/cmdline shows it: each argument and
       a NUL, as much as fits (ps, top, pgrep -f read it). */
    p->cmdline_len = 0;
    for (int k = 0; a->argv[k]; k++) {
        uint32_t n = (uint32_t)strlen(a->argv[k]) + 1;
        if (p->cmdline_len + n > sizeof p->cmdline)
            break;
        memcpy(p->cmdline + p->cmdline_len, a->argv[k], n);
        p->cmdline_len = (uint16_t)(p->cmdline_len + n);
    }
#ifdef TRACE_EXEC
    /* make TRACE=1: every program started, on the screen, for machines
       with no serial port to read a log from (docs/LOG.md, 2026-10-06). */
    kprintf("[exec %d] %s%s%s\n", p->pid, prog, argc > 1 ? " " : "", argc > 1 ? a->argv[1] : "");
#endif
    kfree(a);

    /* Return to user mode at the new program's entry, with its stack, clean
       registers and a clean FPU. */
    struct regs *r = thread_current()->uregs;
#ifdef ARCH_X86_64
    regs_set_ip(r, entry);
    regs_set_sp(r, sp);
    r->rbx = r->rcx = r->rdx = r->rsi = r->rdi = r->rbp = 0;
    r->r8 = r->r9 = r->r10 = r->r11 = r->r12 = r->r13 = r->r14 = r->r15 = 0;
    r->rflags = 0x202;                  /* IF on, nothing else (no stray DF) */
#else
    r->eip = entry;
    r->user_esp = sp;
    r->ebx = r->ecx = r->edx = r->esi = r->edi = r->ebp = 0;
    r->fs = r->gs = GDT_USER_DATA;
#endif
    memset(thread_current()->tls, 0, sizeof thread_current()->tls);
    thread_load_tls(thread_current());  /* no thread-local storage yet */
    thread_start_fpu();
    return 0;                           /* becomes EAX */
}

/* --- exit and wait ------------------------------------------------------------- */

/* The last thread is gone: the process becomes a zombie with p->status. */
static void process_end(struct process *p)
{
#ifdef TRACE_EXEC
    if (W_IFEXITED(p->status))
        kprintf("[exit %d] %s: status %d\n", p->pid, thread_current()->name,
                W_EXITSTATUS(p->status));
    else
        kprintf("[exit %d] %s: signal %d\n", p->pid, thread_current()->name,
                W_TERMSIG(p->status));
#endif
    signal_alarm_cancel(p);
    ptimer_release_all(p);
    sem_exit(p);                        /* SEM_UNDO: give back what it held */
    lock_exit(p);                       /* its POSIX record locks */

    fd_close_all(p);
    vfs_put(p->cwd);
    p->cwd = NULL;
    if (p->root) {
        vfs_put(p->root);
        p->root = NULL;
    }
    proc_clear_exe(p);
    struct mm *mm = p->mm;
    mm_switch(mm_kernel());             /* can't free the space we stand in */
    p->mm = mm_kernel();
    mm_destroy(mm);

    /* Our children get a new parent: init (or nobody). Zombies among them
       must be collected by their new parent — or right away, if none. */
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *c = &procs[i];
        if (c->state == PROC_FREE || c->parent != p)
            continue;
        c->parent = (init_proc && init_proc != p) ? init_proc : NULL;
        if (c->state == PROC_ZOMBIE) {
            if (c->parent)
                wq_wake_all(&c->parent->child_wait);
            else if (!c->kernel_waits)
                reap(c);
        }
    }

    p->state = PROC_ZOMBIE;
    if (p->parent) {
        wq_wake_all(&p->parent->child_wait);
        signal_send(p->parent, SIGCHLD);
    }
    else if (p->kernel_waits)
        wq_wake_all(&kernel_wait);
    else
        reap(p);                        /* an orphan nobody waits for */
}

/* The calling thread ends; the last one out ends the process. */
static void __attribute__((noreturn)) thread_leave(struct process *p)
{
    struct thread *me = thread_current();
    /* pthread_join sleeps on the dying thread's tid word until it's 0. */
    if (me->clear_tid && p->nthreads > 1) {
        uint32_t zero = 0;
        if (copy_to_user((void *)me->clear_tid, &zero, sizeof zero) == 0)
            futex_wake(p->mm, me->clear_tid, 1);
    }
    thread_leave_proc(p, me);
    if (p->nthreads == 0)
        process_end(p);
    else
        wq_wake_all(&p->thread_gone);
    thread_exit();
}

/* exit_group (and fatal signals): every thread of the process ends. The
   others are sent a SIGKILL and die on their way back to user mode; the
   last thread out tears the process down. */
void proc_exit(int status)
{
    struct process *p = proc_current();
    irq_save();
    if (!p->exiting) {
        p->exiting = EXITING_GROUP;
        p->status = status;
        struct thread *me = thread_current();
        for (struct thread *t = p->threads; t; t = t->proc_next)
            if (t != me) {
                t->sig_pending |= SIGBIT(SIGKILL);
                t->sig_check = 1;
                thread_wake(t);
            }
    }
    thread_leave(p);
}

/* exit: only the calling thread ends (pthread_exit, or a thread function
   returning). The last one ends the process with this status. */
void proc_thread_exit(int status)
{
    struct process *p = proc_current();
    irq_save();
    if (p->nthreads == 1)
        proc_exit(status);
    thread_leave(p);
}

#define WNOHANG    1
#define WUNTRACED  2                    /* = waitid's WSTOPPED */
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT  0x01000000             /* waitid: look, but leave the zombie */

/* What a finished child left: wait4 and waitid report it differently. */
struct child_end {
    int pid, status;                    /* status in wait4's encoding */
    uint32_t uid, utime, stime, minflt;
};

/* The waiting wait4 and waitid share: a child that has ended — pid > 0 that
   one, -1 any, 0 one in our process group, < -1 one in group -pid. Fills
   *end and returns 1 (ended), 2 (stopped: WUNTRACED) or 3 (continued:
   WCONTINUED); 0 if none has yet and WNOHANG; or -errno. A stop or a
   continue is told once (unless WNOWAIT), as on Linux. */
static int wait_child(int pid, int options, struct child_end *end)
{
    struct process *me = proc_current();
    for (;;) {
        int have_children = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            struct process *c = &procs[i];
            if (c->state == PROC_FREE || c->parent != me)
                continue;
            if (pid > 0 ? c->pid != pid : pid == 0 ? c->pgid != me->pgid :
                pid < -1 && c->pgid != -pid)
                continue;
            have_children = 1;
            if (c->state == PROC_ALIVE && (options & WUNTRACED) && c->stop_report) {
                *end = (struct child_end){ c->pid, 0x7F | c->stop_report << 8, c->cred.uid,
                                           c->utime, c->stime, 0 };
                if (!(options & WNOWAIT))
                    c->stop_report = 0;
                return 2;
            }
            if (c->state == PROC_ALIVE && (options & WCONTINUED) && c->cont_report) {
                *end = (struct child_end){ c->pid, 0xFFFF, c->cred.uid, c->utime, c->stime, 0 };
                if (!(options & WNOWAIT))
                    c->cont_report = 0;
                return 3;
            }
            if (c->state != PROC_ZOMBIE || !(options & WEXITED))
                continue;
            *end = (struct child_end){ c->pid, c->status, c->cred.uid,
                                       c->utime + c->cutime, c->stime + c->cstime,
                                       c->minflt + c->cminflt };
            if (options & WNOWAIT)
                return 1;
            /* Reap before the caller copies anything out, because the copy
               can sleep: a sibling thread (clone(CLONE_THREAD)) waiting on
               the same child would otherwise see this zombie too, reap it,
               and let a fork reuse the slot — and then our reap would drop
               a *live* process's cwd reference and mark its slot free. The
               status is lost if the copy fails, which is what Linux does
               with a bad pointer as well. */
            me->cutime += end->utime;   /* times(): children's, with theirs */
            me->cstime += end->stime;
            me->cminflt += end->minflt;
            reap(c);
            return 1;
        }
        if (!have_children)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        if (signal_pending())
            return -ERESTARTSYS;
        wq_sleep(&me->child_wait);      /* a child's exit wakes us */
    }
}

int proc_wait4(int pid, uintptr_t user_status, int options, uintptr_t user_rusage)
{
    struct child_end end;
    int found = wait_child(pid, (options & (WNOHANG | WUNTRACED | WCONTINUED)) | WEXITED, &end);
    if (found <= 0)
        return found;
    if (user_status && copy_to_user((void *)user_status, &end.status, 4) < 0)
        return -EFAULT;
    if (user_rusage && rusage_to_user(user_rusage, end.utime, end.stime, end.minflt) < 0)
        return -EFAULT;
    return end.pid;
}

/* waitid(idtype, id, siginfo_t *info, options, rusage): the same, answered
   as a SIGCHLD siginfo — si_status is the exit code or the signal, not
   wait4's packed status. Returns 0; info->si_pid 0 means nobody yet. */
int proc_waitid(int idtype, int id, uintptr_t user_info, int options, uintptr_t user_rusage)
{
    enum { P_ALL, P_PID, P_PGID };
    if (!(options & (WEXITED | WUNTRACED | WCONTINUED)) || idtype > P_PGID ||
        (idtype == P_PID && id <= 0) || id < 0)
        return -EINVAL;
    int pid = idtype == P_ALL ? -1 : idtype == P_PID ? id : id ? -id : 0;
    struct child_end end = { 0 };
    int found = wait_child(pid, options, &end);
    if (found < 0)
        return found;
    struct {                            /* siginfo_t's SIGCHLD fields */
        int32_t signo, err, code;
#ifdef ARCH_X86_64
        int32_t pad;                    /* the union is 8-aligned there */
#endif
        int32_t pid;
        uint32_t uid;
        int32_t status;
    } si = { 0 };
    if (found) {
        int sig = end.status & 0x7F;
        si.signo = SIGCHLD;
        si.code = !sig ? 1 : end.status & 0x80 ? 3 : 2;     /* CLD_EXITED, _DUMPED, _KILLED */
        si.status = sig ? sig : (end.status >> 8) & 0xFF;
        if (found == 2) {               /* CLD_STOPPED: by which signal */
            si.code = 5;
            si.status = (end.status >> 8) & 0xFF;
        } else if (found == 3) {        /* CLD_CONTINUED */
            si.code = 6;
            si.status = SIGCONT;
        }
        si.pid = end.pid;
        si.uid = end.uid;
    }
    if (user_info && copy_to_user((void *)user_info, &si, sizeof si) < 0)
        return -EFAULT;
    if (found && user_rusage && rusage_to_user(user_rusage, end.utime, end.stime, end.minflt) < 0)
        return -EFAULT;
    return 0;
}

/* --- process groups and sessions ------------------------------------------- */

int proc_setpgid(int pid, int pgid)
{
    struct process *me = proc_current();
    struct process *p = pid ? proc_find(pid) : me;
    if (!p || (p != me && p->parent != me))
        return -ESRCH;
    if (pgid < 0)
        return -EINVAL;
    if (p->sid != me->sid || p->pid == p->sid)
        return -EPERM;                  /* other session, or a session leader */
    p->pgid = pgid ? pgid : p->pid;
    return 0;
}

int proc_pgrp_orphaned(int pgid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *c = &procs[i];
        if (c->state != PROC_ALIVE || c->pgid != pgid)
            continue;
        struct process *pp = c->parent;
        if (pp && pp->state == PROC_ALIVE && pp->pgid != pgid && pp->sid == c->sid)
            return 0;                   /* a shell (or the like) can continue it */
    }
    return 1;
}

int proc_session_alive(int sid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_ALIVE && procs[i].sid == sid)
            return 1;
    return 0;
}

int proc_setsid(void)
{
    struct process *p = proc_current();
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state != PROC_FREE && procs[i].pgid == p->pid && &procs[i] != p)
            return -EPERM;              /* already leads a group with members */
    p->sid = p->pgid = p->pid;
    return p->sid;
}
