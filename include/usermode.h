#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>
#include <thread.h>

#define USER_STACK_TOP   0xC0000000u    /* stack grows down from here */
#define USER_STACK_PAGES 16             /* 64 KiB */

/* Load an ELF executable, build the Linux start-up stack (argc, argv, envp,
   auxiliary vector) and start a thread that runs it in ring 3. argv is a
   NULL-terminated list, like main()'s. Returns the thread, or NULL.
   For now all programs share one address space: run one at a time, and
   call user_cleanup() after it has exited. */
struct thread *user_exec(const char *name, const void *elf, uint32_t size,
                         const char *const argv[]);

/* Free all memory of the program user_exec() loaded. */
void user_cleanup(void);

#endif
