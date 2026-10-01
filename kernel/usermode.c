/*
 * usermode.c — load a user program and start it in ring 3
 *
 * Memory for a program (all pages marked "user", so ring 3 may touch them):
 *
 *   0x08048000   code + data   (copied from the embedded flat binary)
 *   0xBFFFF000   one stack page, just below the kernel at 0xC0000000
 *
 * Shortcut for now: there is one page directory for everything, so every
 * program would use the same addresses. We only run one program at a time
 * and remove its pages afterwards. Per-process page directories come with
 * fork/exec (step 8).
 */
#include <memlayout.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <usermode.h>

void user_enter(uint32_t eip, uint32_t esp);    /* usermode.asm */

static uint32_t code_pages;                     /* how much we mapped */

/* Map `n` fresh, zeroed user pages starting at `va`. */
static int map_user_pages(uint32_t va, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t frame = pmm_alloc();
        if (!frame)
            return -1;
        memset(P2V(frame), 0, PAGE_SIZE);
        if (vmm_map(va + i * PAGE_SIZE, frame, PTE_USER | PTE_WRITABLE) < 0) {
            pmm_free(frame);
            return -1;
        }
    }
    return 0;
}

static void unmap_user_pages(uint32_t va, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t frame = vmm_unmap(va + i * PAGE_SIZE);
        if (frame)
            pmm_free(frame);
    }
}

/* The kernel thread's job: jump into the program. Never comes back here —
   the program leaves through exit() or by being killed. */
static void run_program(void *arg)
{
    (void)arg;
    user_enter(USER_CODE_BASE, USER_STACK_TOP);
}

struct thread *user_spawn(const char *name, const void *code, uint32_t size)
{
    code_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (map_user_pages(USER_CODE_BASE, code_pages) < 0 ||
        map_user_pages(USER_STACK_TOP - PAGE_SIZE, 1) < 0) {
        user_cleanup();
        return NULL;
    }
    /* The pages are mapped in the (shared) page directory, so the kernel
       can write the program straight to its user address. */
    memcpy((void *)USER_CODE_BASE, code, size);

    struct thread *t = thread_create(name, run_program, NULL);
    if (!t)
        user_cleanup();
    return t;
}

void user_cleanup(void)
{
    unmap_user_pages(USER_CODE_BASE, code_pages);
    unmap_user_pages(USER_STACK_TOP - PAGE_SIZE, 1);
    code_pages = 0;
}
