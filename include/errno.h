#ifndef ERRNO_H
#define ERRNO_H

/* Linux errno values (asm-generic/errno-base.h). Syscalls return -ERRNO
   on failure; the C library turns that into -1 and sets errno. They must
   match Linux, because compiled programs compare against these numbers. */
#define ENOEXEC   8     /* not an executable we can run */
#define EBADF     9     /* bad file descriptor */
#define ENOMEM   12     /* out of memory */
#define EFAULT   14     /* bad address */
#define EINVAL   22     /* invalid argument */
#define ENOTTY   25     /* not a terminal */
#define ENOSYS   38     /* function not implemented */

#endif
