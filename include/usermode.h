#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>
#include <thread.h>

#define USER_STACK_TOP   0xBFFFF000u    /* stack grows down from here; the
                                           page above holds the vsyscall
                                           entry (arch/i386/sysenter.h) */
#define USER_STACK_PAGES 16             /* 64 KiB */

/* Load the ELF executable at `path` (in the VFS), build the Linux start-up
   stack (argc, argv, envp, auxiliary vector), open fds 0-2 on the console,
   and start a thread that runs it in ring 3. argv is a NULL-terminated
   list, like main()'s. Returns the thread, or NULL.
   For now all programs share one address space: run one at a time, and
   call user_cleanup() after it has exited. */
struct thread *user_exec(const char *path, const char *const argv[]);

/* Close the program's files and free all its memory. */
void user_cleanup(struct thread *t);

#endif
