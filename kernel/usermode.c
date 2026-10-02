/*
 * usermode.c — load a program into an address space
 *
 * Used by execve() and by the kernel when it starts the first program. The
 * program goes into the CURRENT address space (which should be empty):
 *
 *   0x08048000   code + data   (the ELF file's PT_LOAD segments, elf.c),
 *                then the heap (brk) right after them
 *   0xB7FFF000   the stack area: up to 8 MiB, pages appear when touched
 *   0xBFFFF000   the vsyscall page: the fast way into the kernel (sysenter),
 *                read-only, shared by every program
 *   0xC0000000   the kernel
 *
 * The stack doesn't start empty. Linux hands every program its arguments
 * and environment and some facts about itself on the stack; the C library's
 * _start reads them before calling main(). At the moment the program starts:
 *
 *     ESP ->  argc
 *             argv[0] ... argv[argc-1], NULL     (pointers to strings)
 *             envp[0] ... envp[envc-1], NULL     (the environment)
 *             auxv: (type, value) pairs ... (AT_NULL, 0)
 *             ... the strings themselves, 16 random bytes ...
 *     0xBFFFF000
 *
 * The "auxiliary vector" (auxv) tells the program things only the kernel
 * knows: the page size, where its program headers ended up in memory
 * (musl needs them for thread-local storage), random bytes for security
 * features, user/group IDs, and where the fast syscall entry is.
 */
#include <elf.h>
#include <errno.h>
#include <kheap.h>
#include <memlayout.h>
#include <mm.h>
#include <string.h>
#include <sysenter.h>
#include <timer.h>
#include <usermode.h>
#include <vfs.h>

/* Auxiliary vector types (Linux <elf.h>) */
#define AT_NULL    0
#define AT_IGNORE  1
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7            /* the interpreter's base: 0 = none */
#define AT_ENTRY   9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_SYSINFO 32           /* the vsyscall entry (sysenter) */

/* The most execve() accepts for all argument and environment strings
   together (Linux allows more; this is plenty for a shell). */
#define ARG_SPACE (64 * 1024)

static uint32_t count(const char *const v[])
{
    uint32_t n = 0;
    while (v && v[n])
        n++;
    return n;
}

/* Copy n strings onto the stack (growing down), and their addresses
   into addr[]. */
static uint32_t push_strings(uint32_t sp, const char *const v[], uint32_t n,
                             uint32_t *addr)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = strlen(v[i]) + 1;
        sp -= len;
        memcpy((void *)sp, v[i], len);
        addr[i] = sp;
    }
    return sp;
}

/* Build the start-up stack described at the top; returns the initial ESP.
   The kernel writes straight into the new program's stack: its pages
   appear on first touch (demand paging), like the program's own would. */
static uint32_t build_stack(const char *const argv[], const char *const envp[],
                            const struct elf_info *info, uint32_t sysinfo,
                            uint32_t interp_base)
{
    uint32_t argc = count(argv), envc = count(envp);
    uint32_t sp = USER_STACK_TOP;

    /* 1. 16 "random" bytes for AT_RANDOM (not really random yet: there's no
       entropy source). The strings go below them, the table below those. */
    uint32_t seed = timer_ms() * 2654435761u, rnd[4];
    for (int i = 0; i < 4; i++)
        rnd[i] = seed = seed * 1103515245 + 12345;
    sp -= sizeof rnd;
    memcpy((void *)sp, rnd, sizeof rnd);
    uint32_t random_addr = sp;

    uint32_t auxv[] = {
        AT_PHDR,   info->phdr,
        AT_PHENT,  sizeof(struct elf32_phdr),
        AT_PHNUM,  info->phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_BASE,   interp_base,         /* where the dynamic linker is (0: none) */
        AT_ENTRY,  info->entry,
        AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,   /* we're all root */
        AT_SECURE, 0,
        AT_RANDOM, random_addr,
        sysinfo ? AT_SYSINFO : AT_IGNORE, sysinfo,
        AT_NULL,   0,
    };

    /* 2. The table: argc, argv..., 0, envp..., 0, auxv. It sits below the
       space the strings may use; the i386 ABI wants ESP 16-byte aligned at
       the program's entry. */
    uint32_t words = 1 + argc + 1 + envc + 1 + sizeof auxv / 4;
    uint32_t *table = (uint32_t *)((sp - ARG_SPACE - words * 4) & ~0xFu);

    uint32_t *p = table;
    *p++ = argc;
    sp = push_strings(sp, argv, argc, p);
    p += argc;
    *p++ = 0;                           /* end of argv */
    push_strings(sp, envp, envc, p);
    p += envc;
    *p++ = 0;                           /* end of envp */
    memcpy(p, auxv, sizeof auxv);
    return (uint32_t)table;
}

/* "#!/bin/sh -x" at the top of a file: run the interpreter instead, with
   the script's path as an argument. Fills interp/arg; 1 if it's a script. */
static int script_interp(const struct inode *ino, char *interp, char *arg)
{
    if (ino->size < 3 || ino->data[0] != '#' || ino->data[1] != '!')
        return 0;
    uint32_t i = 2, n = 0;
    while (i < ino->size && (ino->data[i] == ' ' || ino->data[i] == '\t'))
        i++;
    while (i < ino->size && n < PATH_MAX - 1 && ino->data[i] > ' ')
        interp[n++] = ino->data[i++];
    interp[n] = '\0';
    while (i < ino->size && (ino->data[i] == ' ' || ino->data[i] == '\t'))
        i++;
    n = 0;
    while (i < ino->size && n < PATH_MAX - 1 && ino->data[i] != '\n')
        arg[n++] = ino->data[i++];
    while (n > 0 && arg[n - 1] <= ' ')
        n--;                            /* trailing spaces / \r */
    arg[n] = '\0';
    return interp[0] != '\0';
}

static int load_elf(struct inode *ino, const char *const argv[],
                    const char *const envp[], uint32_t *entry, uint32_t *sp);

int program_load(const char *path, const char *const argv[],
                 const char *const envp[], uint32_t *entry, uint32_t *sp)
{
    /* The program's file is in the initramfs, already in memory: the
       loader reads it right where it is (no copy). */
    struct inode *ino;
    int err = vfs_lookup(path, &ino);
    if (err)
        return err;
    if (S_ISDIR(ino->mode))
        return -EISDIR;
    if (!S_ISREG(ino->mode) || !ino->data)
        return -EACCES;

    /* A script? Then run "interpreter [arg] script args..." instead. */
    struct script {
        char interp[PATH_MAX], arg[PATH_MAX];
        const char *argv[];
    } *s = kmalloc(sizeof(struct script) + (count(argv) + 4) * sizeof(char *));
    if (!s)
        return -ENOMEM;
    if (script_interp(ino, s->interp, s->arg)) {
        int n = 0;
        s->argv[n++] = s->interp;
        if (s->arg[0])
            s->argv[n++] = s->arg;
        s->argv[n++] = path;
        for (uint32_t i = 1; argv && argv[i]; i++)
            s->argv[n++] = argv[i];
        s->argv[n] = NULL;
        struct inode *interp;
        err = vfs_lookup(s->interp, &interp);
        if (!err && (!S_ISREG(interp->mode) || !interp->data))
            err = -EACCES;
        if (!err)                       /* (one level: no #! interpreters) */
            err = load_elf(interp, s->argv, envp, entry, sp);
        kfree(s);
        return err;
    }
    kfree(s);
    return load_elf(ino, argv, envp, entry, sp);
}

static int load_elf(struct inode *ino, const char *const argv[],
                    const char *const envp[], uint32_t *entry, uint32_t *sp)
{
    int err;

    struct elf_info info;
    err = elf_load(ino->data, ino->size, ELF_DYN_BASE, &info);
    if (err)
        return err;

    struct mm *mm = mm_current();
    mm->brk_start = mm->brk = info.brk_start;

    /* Dynamically linked? Load its dynamic linker too (into the mmap area)
       and start THAT: it reads the auxiliary vector to find the program
       (AT_PHDR, AT_ENTRY), loads the shared libraries, and jumps to the
       program's entry. */
    uint32_t start = info.entry, interp_base = 0;
    if (info.interp) {
        struct inode *ld;
        err = vfs_lookup(info.interp, &ld);
        if (err)
            return err == -ENOENT ? -ENOENT : err;
        if (!S_ISREG(ld->mode) || !ld->data)
            return -EACCES;
        struct elf_info linfo;
        err = elf_load(ld->data, ld->size, ELF_BASE_ANY, &linfo);
        if (err)
            return err;
        if (linfo.interp)
            return -ENOEXEC;            /* an interpreter needing one: no */
        start = linfo.entry;
        interp_base = linfo.base;
    }
    err = mm_add(mm, USER_STACK_BOTTOM, USER_STACK_TOP, VM_READ | VM_WRITE);
    if (err)
        return err;

    uint32_t sysinfo = vsyscall_map();  /* 0: CPU has no sysenter */
    *entry = start;
    *sp = build_stack(argv, envp, &info, sysinfo, interp_base);
    return 0;
}
