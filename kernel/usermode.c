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
#include <random.h>
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

static uint32_t strings_size(const char *const v[], uint32_t n)
{
    uint32_t size = 0;
    for (uint32_t i = 0; i < n; i++)
        size += strlen(v[i]) + 1;
    return size;
}

/* Copy n strings to `at` and up, one after another, and their addresses
   into addr[]. Returns where the next one would go. */
static user_addr_t put_strings(user_addr_t at, const char *const v[], uint32_t n,
                               user_addr_t *addr)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = strlen(v[i]) + 1;
        memcpy((void *)(uintptr_t)at, v[i], len);
        addr[i] = at;
        at += len;
    }
    return at;
}

/* The size of one program header, for AT_PHENT. */
#ifdef ARCH_X86_64
#define PHENT_SIZE 56                   /* Elf64_Phdr */
#else
#define PHENT_SIZE sizeof(struct elf32_phdr)
#endif

/* Build the start-up stack described at the top; returns the initial stack
   pointer. Every entry of the table is one machine word (user_addr_t): 4
   bytes on i386, 8 on x86_64. The kernel writes straight into the new
   program's stack: its pages appear on first touch (demand paging), like
   the program's own would. */
static user_addr_t build_stack(const char *const argv[], const char *const envp[],
                               const struct elf_info *info, user_addr_t sysinfo,
                               user_addr_t interp_base, const struct cred *c)
{
    uint32_t argc = count(argv), envc = count(envp);
    user_addr_t sp = USER_STACK_TOP;

    /* 1. 16 random bytes for AT_RANDOM. musl uses these to pick its stack
       guard value and to seed its hash tables, so a predictable 16 bytes
       hands an attacker the stack canary. The strings go below them, the
       table below those. */
    uint32_t rnd[4];
    random_bytes(rnd, sizeof rnd);
    sp -= sizeof rnd;
    memcpy((void *)(uintptr_t)sp, rnd, sizeof rnd);
    user_addr_t random_addr = sp;

    user_addr_t auxv[] = {
        AT_PHDR,   info->phdr,
        AT_PHENT,  PHENT_SIZE,
        AT_PHNUM,  info->phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_BASE,   interp_base,         /* where the dynamic linker is (0: none) */
        AT_ENTRY,  info->entry,
        AT_UID, c->uid, AT_EUID, c->euid, AT_GID, c->gid, AT_EGID, c->egid,
        /* A setuid program: its C library must not trust the environment
           (LD_PRELOAD, LD_LIBRARY_PATH) of the user who started it. */
        AT_SECURE, c->uid != c->euid || c->gid != c->egid,
        AT_RANDOM, random_addr,
        sysinfo ? AT_SYSINFO : AT_IGNORE, sysinfo,
        AT_NULL,   0,
    };

    /* 2. The table: argc, argv..., 0, envp..., 0, auxv. It sits below the
       space the strings may use; the i386 ABI wants ESP 16-byte aligned at
       the program's entry. */
    uint32_t words = 1 + argc + 1 + envc + 1 + sizeof auxv / sizeof auxv[0];
    user_addr_t *table = (user_addr_t *)(uintptr_t)
        ((sp - ARG_SPACE - words * sizeof(user_addr_t)) & ~(user_addr_t)0xF);

    /* The strings in Linux's order: argv[0] lowest, then the rest of argv,
       then the environment, all in one run. Programs that rewrite their
       name for `ps` (util-linux's login, sendmail) treat argv[0] through
       the end of the last environment string as one buffer to overwrite. */
    user_addr_t *p = table;
    *p++ = argc;
    user_addr_t at = sp - strings_size(argv, argc) - strings_size(envp, envc);
    at = put_strings(at, argv, argc, p);
    p += argc;
    *p++ = 0;                           /* end of argv */
    put_strings(at, envp, envc, p);
    p += envc;
    *p++ = 0;                           /* end of envp */
    memcpy(p, auxv, sizeof auxv);
    return (user_addr_t)(uintptr_t)table;
}

/* "#!/bin/sh -x" at the top of a file: run the interpreter instead, with
   the script's path as an argument. Only the first line counts, and only
   its first SCRIPT_HEAD bytes (Linux reads 256 too). Fills interp/arg;
   1 if it's a script. */
#define SCRIPT_HEAD 256

static int script_interp(struct inode *ino, char *head, char *interp, char *arg)
{
    uint32_t size = ino->size < SCRIPT_HEAD ? ino->size : SCRIPT_HEAD;
    if (size < 3)
        return 0;
    file_copy_out(ino, 0, head, 2);     /* most programs aren't scripts: */
    if (head[0] != '#' || head[1] != '!')
        return 0;                       /* look at 2 bytes, not 256 */
    file_copy_out(ino, 0, head, size);
    uint32_t i = 2, n = 0;
    while (i < size && (head[i] == ' ' || head[i] == '\t'))
        i++;
    while (i < size && head[i] > ' ')
        interp[n++] = head[i++];
    interp[n] = '\0';
    while (i < size && (head[i] == ' ' || head[i] == '\t'))
        i++;
    n = 0;
    while (i < size && head[i] != '\n')
        arg[n++] = head[i++];
    while (n > 0 && arg[n - 1] <= ' ')
        n--;                            /* trailing spaces / \r */
    arg[n] = '\0';
    return interp[0] != '\0';
}

static int load_elf(struct inode *ino, const char *const argv[],
                    const char *const envp[], user_addr_t *entry, user_addr_t *sp,
                    struct cred *cred);

int program_load(const char *path, const char *const argv[],
                 const char *const envp[], user_addr_t *entry, user_addr_t *sp,
                 struct cred *cred, struct inode **exe_out)
{
    if (exe_out)
        *exe_out = NULL;
    /* A program from the initramfs is already in memory: the loader
       reads it right where it is (no copy). */
    struct inode *ino;
    int err = vfs_lookup(path, &ino);
    if (err)
        return err;
    if (S_ISDIR(ino->mode))
        return -EISDIR;
    if (!S_ISREG(ino->mode) || (ino->ops && !ino->fs) || !(ino->mode & 0111))
        return -EACCES;

    /* A script? Then run "interpreter [arg] script args..." instead. */
    struct script {
        char head[SCRIPT_HEAD], interp[SCRIPT_HEAD], arg[SCRIPT_HEAD];
        const char *argv[];
    } *s = kmalloc(sizeof(struct script) + (count(argv) + 4) * sizeof(char *));
    if (!s)
        return -ENOMEM;
    struct inode *exe = ino;
    if (script_interp(ino, s->head, s->interp, s->arg)) {
        int n = 0;
        s->argv[n++] = s->interp;
        if (s->arg[0])
            s->argv[n++] = s->arg;
        s->argv[n++] = path;
        for (uint32_t i = 1; argv && argv[i]; i++)
            s->argv[n++] = argv[i];
        s->argv[n] = NULL;
        struct inode *interp = NULL;
        err = vfs_lookup(s->interp, &interp);
        if (!err && (!S_ISREG(interp->mode) || (interp->ops && !interp->fs)))
            err = -EACCES;
        if (!err)                       /* (one level: no #! interpreters) */
            err = load_elf(interp, s->argv, envp, entry, sp, cred);
        kfree(s);
        if (!err)
            exe = interp;               /* /proc/self/exe is the interpreter */
    } else {
        kfree(s);
        err = load_elf(ino, argv, envp, entry, sp, cred);
    }
    if (!err && exe_out) {
        vfs_get(exe);
        *exe_out = exe;
    }
    return err;
}

static int load_elf(struct inode *ino, const char *const argv[],
                    const char *const envp[], user_addr_t *entry, user_addr_t *sp,
                    struct cred *cred)
{
    int err;

    /* Map the file. Pages are read when the program touches them (and the
       next page is read ahead), so a binary on disk is not copied whole. */
    struct elf_info info;
    char interp[PATH_MAX];
    err = elf_map(ino, ELF_DYN_BASE, &info, interp, sizeof interp);
    if (err)
        return err;

    struct mm *mm = mm_current();
    mm->brk_start = mm->brk = info.brk_start;

    /* Dynamically linked? Load its dynamic linker too (into the mmap area)
       and start THAT: it reads the auxiliary vector to find the program
       (AT_PHDR, AT_ENTRY), loads the shared libraries, and jumps to the
       program's entry. */
    user_addr_t start = info.entry, interp_base = 0;
    if (info.interp) {
        struct inode *ld;
        err = vfs_lookup(info.interp, &ld);
        if (err)
            return err == -ENOENT ? -ENOENT : err;
        if (!S_ISREG(ld->mode) || (ld->ops && !ld->fs))
            return -EACCES;
        struct elf_info linfo;
        err = elf_map(ld, ELF_BASE_ANY, &linfo, interp, sizeof interp);
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

    /* A setuid / setgid file runs as its owner / group. (Setgid without
       group execute permission means something else, so it's skipped, as
       in Linux.) The saved ids always take the effective ones. */
    if (ino->mode & S_ISUID)
        cred->euid = ino->uid;
    if ((ino->mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
        cred->egid = ino->gid;
    cred->suid = cred->fsuid = cred->euid;
    cred->sgid = cred->fsgid = cred->egid;

    user_addr_t sysinfo = vsyscall_map();   /* 0: CPU has no sysenter */
    *entry = start;
    *sp = build_stack(argv, envp, &info, sysinfo, interp_base, cred);
    return 0;
}
