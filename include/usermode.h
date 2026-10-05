#ifndef USERMODE_H
#define USERMODE_H

#include <cred.h>
#include <stdint.h>

struct inode;

/* Load the ELF executable at `path` (in the VFS) into the CURRENT address
   space (which should be empty), and build the Linux start-up stack: argc,
   argv, envp, auxiliary vector. argv and envp are NULL-terminated lists in
   kernel memory. Returns 0 and where to start (entry, stack pointer), or
   -errno. `cred` comes in as the caller's credentials and goes out as the
   new program's (a setuid file changes them); the caller installs them
   once nothing can fail any more. On success `*exe_out` (if not NULL) is
   the ELF that was loaded — the interpreter for a #! script — with a
   reference the caller must vfs_put. That is /proc/self/exe. */
int program_load(const char *path, const char *const argv[],
                 const char *const envp[], uint32_t *entry, uint32_t *sp,
                 struct cred *cred, struct inode **exe_out);

/* Jump to user mode at eip with stack esp (usermode.asm). Never returns. */
#ifdef ARCH_X86_64
void user_enter(uint64_t rip, uint64_t rsp) __attribute__((noreturn));
#else
void user_enter(uint32_t eip, uint32_t esp) __attribute__((noreturn));
#endif

#endif
