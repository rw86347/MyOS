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
#include <gdt.h>
#include <interrupts.h>
#include <kheap.h>
#include <kprintf.h>
#include <mm.h>
#include <proc.h>
#include <string.h>
#include <thread.h>
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
            p->pid = next_pid++;
            p->state = PROC_ALIVE;
            p->cwd = vfs_root();
            vfs_get(p->cwd);
            p->umask = 022;
            p->pgid = p->sid = p->pid;  /* its own group and session */
            return p;
        }
    }
    return NULL;
}

static void reap(struct process *p)
{
    if (p->cwd) {                       /* a process that never ran */
        vfs_put(p->cwd);
        p->cwd = NULL;
    }
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
    user_enter(p->start_eip, p->start_esp);
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
    int err = program_load(path, argv, envp, &p->start_eip, &p->start_esp);
    mm_switch(old);
    if (err) {
        mm_destroy(p->mm);
        reap(p);
        return NULL;
    }

    fd_open_console(p);                 /* stdin, stdout, stderr */
    p->kernel_waits = 1;
    p->thread = thread_create_in(p, base_name(path), start_user, p);
    if (!p->thread) {
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

/* --- fork ----------------------------------------------------------------- */

int proc_fork(void)
{
    return proc_fork_frame(thread_current()->uregs, NULL);
}

#define CLONE_THREAD        0x00010000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000

int proc_clone(uint32_t flags, uint32_t new_sp, uint32_t parent_tid, uint32_t tls)
{
    if (flags & CLONE_THREAD)
        return -ENOSYS;                 /* threads in one process: later */
    struct regs frame = *thread_current()->uregs;
    if (new_sp)
        frame.user_esp = new_sp;        /* the child runs on its own stack */
    struct process *child;
    int pid = proc_fork_frame(&frame, &child);
    if (pid < 0)
        return pid;
    if (flags & CLONE_SETTLS) {         /* (a user_desc, as set_thread_area) */
        uint32_t d[4];
        if (copy_from_user(d, (void *)tls, sizeof d) == 0 &&
            d[0] >= GDT_TLS_FIRST && d[0] < GDT_TLS_FIRST + GDT_TLS_ENTRIES)
            child->thread->tls[d[0] - GDT_TLS_FIRST] = gdt_tls_descriptor(d[1], d[2], d[3]);
    }
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
    child->umask = parent->umask;
    child->pgid = parent->pgid;         /* same job, same session */
    child->sid = parent->sid;
    signal_fork(child, parent);
    child->cloexec = parent->cloexec;
    fd_copy_all(child, parent);         /* same open files, shared */

    /* The child's thread starts by "returning" from this very syscall,
       with the parent's registers — except EAX = 0. */
    child->thread = thread_fork(child, me->name, frame);
    if (!child->thread) {
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
static int copy_strings(struct exec_args *a, char **out, int room, uint32_t user_vec)
{
    if (!user_vec)
        return 0;                       /* NULL argv/envp: treat as empty */
    for (int n = 0; ; n++) {
        uint32_t ptr;
        if (get_user32(&ptr, user_vec + n * 4) < 0)
            return -EFAULT;
        if (!ptr) {
            out[n] = NULL;
            return n;
        }
        if (n == room)
            return -E2BIG;
        long len = strncpy_from_user(a->str + a->used, (const char *)ptr,
                                     EXEC_STR_SPACE - a->used);
        if (len == -ENAMETOOLONG)
            return -E2BIG;
        if (len < 0)
            return len;
        out[n] = a->str + a->used;
        a->used += len + 1;
    }
}

int proc_execve(uint32_t user_path, uint32_t user_argv, uint32_t user_envp)
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
    int argc = err < 0 ? (int)err : copy_strings(a, a->argv, EXEC_MAX_ARGS, user_argv);
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
    uint32_t entry, sp;
    err = program_load(prog, argv, (const char *const *)a->envp, &entry, &sp);
    if (err) {
        mm_switch(old_mm);
        mm_destroy(new_mm);
        kfree(a);
        return err;
    }

    /* 3. Point of no return: the old program is gone. */
    p->mm = new_mm;
    mm_destroy(old_mm);
    for (int fd = 0; fd < PROC_FILES; fd++)
        if (p->cloexec & (1u << fd))
            fd_close(fd);               /* close-on-exec files */
    p->cloexec = 0;
    signal_exec(p);                     /* handlers were in the old program */
    thread_set_name(thread_current(), base_name(prog));
    kfree(a);

    /* Return to user mode at the new program's entry, with its stack, clean
       registers and a clean FPU. */
    struct regs *r = thread_current()->uregs;
    r->eip = entry;
    r->user_esp = sp;
    r->ebx = r->ecx = r->edx = r->esi = r->edi = r->ebp = 0;
    r->fs = r->gs = GDT_USER_DATA;      /* the old TLS segment is gone */
    memset(thread_current()->tls, 0, sizeof thread_current()->tls);
    thread_load_tls(thread_current());  /* no thread-local storage yet */
    thread_start_fpu();
    return 0;                           /* becomes EAX */
}

/* --- exit and wait ------------------------------------------------------------- */

void proc_exit(int status)
{
    struct process *p = proc_current();
    irq_save();
    signal_alarm_cancel(p);

    fd_close_all(p);
    vfs_put(p->cwd);
    p->cwd = NULL;
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

    p->status = status;
    p->state = PROC_ZOMBIE;
    if (p->parent) {
        wq_wake_all(&p->parent->child_wait);
        signal_send(p->parent, SIGCHLD);
    }
    else if (p->kernel_waits)
        wq_wake_all(&kernel_wait);
    else
        reap(p);                        /* an orphan nobody waits for */
    thread_exit();
}

#define WNOHANG 1

int proc_wait4(int pid, uint32_t user_status, int options)
{
    struct process *me = proc_current();
    for (;;) {
        int have_children = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            struct process *c = &procs[i];
            if (c->state == PROC_FREE || c->parent != me)
                continue;
            if (pid > 0 && c->pid != pid)
                continue;               /* (pid 0 / < -1 = process groups:
                                            any child, for now) */
            have_children = 1;
            if (c->state == PROC_ZOMBIE) {
                if (user_status && copy_to_user((void *)user_status, &c->status, 4) < 0)
                    return -EFAULT;
                int id = c->pid;
                reap(c);
                return id;
            }
        }
        if (!have_children)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        if (signal_pending())
            return -EINTR;
        wq_sleep(&me->child_wait);      /* a child's exit wakes us */
    }
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

int proc_setsid(void)
{
    struct process *p = proc_current();
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state != PROC_FREE && procs[i].pgid == p->pid && &procs[i] != p)
            return -EPERM;              /* already leads a group with members */
    p->sid = p->pgid = p->pid;
    return p->sid;
}
