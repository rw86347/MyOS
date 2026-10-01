/*
 * usermode.c — load a user program and start it in ring 3
 *
 * Memory for a program (all pages marked "user", so ring 3 may touch them):
 *
 *   0x08048000   code + data   (the ELF file's PT_LOAD segments, elf.c)
 *   ...
 *   0xBFFEF000   stack, 64 KiB, growing down from 0xBFFFF000
 *   0xBFFFF000   the vsyscall page: the fast way into the kernel (sysenter),
 *                read-only, shared by every program
 *   0xC0000000   the kernel
 *
 * The stack doesn't start empty. Linux hands every program its arguments
 * and some facts about itself on the stack; the C library's _start reads
 * them before calling main(). At the moment the program starts:
 *
 *     ESP ->  argc
 *             argv[0] ... argv[argc-1], NULL     (pointers to strings)
 *             envp[0] ... NULL                   (environment; empty for now)
 *             auxv: (type, value) pairs ... (AT_NULL, 0)
 *             ... the strings themselves, 16 random bytes ...
 *     0xC0000000
 *
 * The "auxiliary vector" (auxv) tells the program things only the kernel
 * knows: the page size, where its program headers ended up in memory
 * (musl needs them for thread-local storage), random bytes for security
 * features, user/group IDs, and where the fast syscall entry is.
 *
 * Shortcut for now: there is one page directory for everything, so every
 * program would use the same addresses. We only run one program at a time
 * and remove its pages afterwards. Per-process page directories come with
 * fork/exec (step 8).
 */
#include <elf.h>
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <sysenter.h>
#include <timer.h>
#include <usermode.h>
#include <vfs.h>

void user_enter(uint32_t eip, uint32_t esp);    /* usermode.asm */

/* Auxiliary vector types (Linux <elf.h>) */
#define AT_NULL    0
#define AT_IGNORE  1
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_ENTRY   9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_SYSINFO 32           /* the vsyscall entry (sysenter) */

#define MAX_ARGS 16

/* Where the new program starts. One program at a time, so static is fine. */
static uint32_t start_eip, start_esp;

static int map_stack(void)
{
    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        uint32_t frame = pmm_alloc();
        if (!frame)
            return -1;
        memset(P2V(frame), 0, PAGE_SIZE);
        if (vmm_map(USER_STACK_TOP - i * PAGE_SIZE, frame, PTE_USER | PTE_WRITABLE) < 0) {
            pmm_free(frame);
            return -1;
        }
    }
    return 0;
}

/* Copy bytes onto the user stack (growing down); returns their address. */
static uint32_t push_bytes(uint32_t *sp, const void *data, uint32_t len)
{
    *sp -= len;
    memcpy((void *)*sp, data, len);
    return *sp;
}

/* Build the start-up stack described at the top; returns the initial ESP. */
static uint32_t build_stack(const char *const argv[], const struct elf_info *info,
                            uint32_t sysinfo)
{
    uint32_t sp = USER_STACK_TOP;

    /* 1. The strings, and 16 "random" bytes for AT_RANDOM. (Not really
       random yet: there's no entropy source. Good enough to boot.) */
    uint32_t argc = 0, argv_addr[MAX_ARGS];
    while (argc < MAX_ARGS && argv[argc]) {
        argv_addr[argc] = push_bytes(&sp, argv[argc], strlen(argv[argc]) + 1);
        argc++;
    }
    uint32_t seed = timer_ms() * 2654435761u, rnd[4];
    for (int i = 0; i < 4; i++)
        rnd[i] = seed = seed * 1103515245 + 12345;
    uint32_t random_addr = push_bytes(&sp, rnd, sizeof rnd);

    /* 2. The pointer part, built in a local array, then copied below the
       strings. Order (low to high): argc, argv..., 0, envp 0, auxv... */
    uint32_t auxv[] = {
        AT_PHDR,   info->phdr,
        AT_PHENT,  sizeof(struct elf32_phdr),
        AT_PHNUM,  info->phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_ENTRY,  info->entry,
        AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,   /* we're all root */
        AT_SECURE, 0,
        AT_RANDOM, random_addr,
        sysinfo ? AT_SYSINFO : AT_IGNORE, sysinfo,
        AT_NULL,   0,
    };
    uint32_t words[1 + MAX_ARGS + 1 + 1 + sizeof auxv / 4];
    uint32_t n = 0;
    words[n++] = argc;
    for (uint32_t i = 0; i < argc; i++)
        words[n++] = argv_addr[i];
    words[n++] = 0;                     /* end of argv */
    words[n++] = 0;                     /* envp: empty for now */
    memcpy(&words[n], auxv, sizeof auxv);
    n += sizeof auxv / 4;

    /* The i386 ABI wants ESP 16-byte aligned at the program's entry. */
    sp = (sp - n * 4) & ~0xFu;
    memcpy((void *)sp, words, n * 4);
    return sp;
}

/* The kernel thread's job: jump into the program. Never comes back here —
   the program leaves through exit() or by being killed. */
static void run_program(void *arg)
{
    (void)arg;
    thread_start_fpu();             /* programs may use floats / SSE */
    user_enter(start_eip, start_esp);
}

struct thread *user_exec(const char *path, const char *const argv[])
{
    /* The program's ELF file is in the initramfs, already in memory: the
       loader reads it right where it is (no copy). */
    struct inode *ino;
    if (vfs_lookup(path, &ino) < 0 || !S_ISREG(ino->mode) || !ino->data)
        return NULL;

    struct elf_info info;
    if (elf_load(ino->data, ino->size, &info) < 0 || map_stack() < 0) {
        vmm_unmap_user_all();
        return NULL;
    }
    uint32_t sysinfo = vsyscall_map();      /* 0: CPU has no sysenter */
    start_eip = info.entry;
    start_esp = build_stack(argv, &info, sysinfo);

    /* The thread is named after the file: "/bin/hello" -> "hello". */
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            name = p + 1;

    struct thread *t = thread_create(name, run_program, NULL);
    if (!t) {
        vmm_unmap_user_all();
        return NULL;
    }
    fd_open_console(t);             /* stdin, stdout, stderr */
    return t;
}

void user_cleanup(struct thread *t)
{
    fd_close_all(t);
    vmm_unmap_user_all();
}
