#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

/* Load the ELF executable at `path` (in the VFS) into the CURRENT address
   space (which should be empty), and build the Linux start-up stack: argc,
   argv, envp, auxiliary vector. argv and envp are NULL-terminated lists in
   kernel memory. Returns 0 and where to start (entry, stack pointer), or
   -errno. */
int program_load(const char *path, const char *const argv[],
                 const char *const envp[], uint32_t *entry, uint32_t *sp);

/* Jump to user mode at eip with stack esp (usermode.asm). Never returns. */
void user_enter(uint32_t eip, uint32_t esp) __attribute__((noreturn));

#endif
