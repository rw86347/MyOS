#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>
#include <thread.h>

/* Where user programs live (Linux i386 uses the same addresses). */
#define USER_CODE_BASE  0x08048000u
#define USER_STACK_TOP  0xC0000000u     /* stack grows down from here */

/* Copy a flat binary to USER_CODE_BASE, give it a stack, and start a thread
   that runs it in ring 3. Returns the thread, or NULL on failure.
   For now all programs share one address space: run one at a time, and
   call user_cleanup() after it has exited. */
struct thread *user_spawn(const char *name, const void *code, uint32_t size);

/* Unmap and free the memory of the program user_spawn() loaded. */
void user_cleanup(void);

#endif
