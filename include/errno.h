#ifndef ERRNO_H
#define ERRNO_H

/* Linux errno values (asm-generic/errno-base.h). Syscalls return -ERRNO
   on failure; the C library turns that into -1 and sets errno. They must
   match Linux, because compiled programs compare against these numbers. */
#define ENOENT    2     /* no such file or directory */
#define ENOEXEC   8     /* not an executable we can run */
#define EBADF     9     /* bad file descriptor */
#define ENOMEM   12     /* out of memory */
#define EFAULT   14     /* bad address */
#define ENOTDIR  20     /* a path component is not a directory */
#define EISDIR   21     /* is a directory */
#define EINVAL   22     /* invalid argument */
#define EMFILE   24     /* too many open files */
#define ENOTTY   25     /* not a terminal */
#define ESPIPE   29     /* can't seek (e.g. the console) */
#define EROFS    30     /* read-only file system */
#define ENAMETOOLONG 36 /* file name too long */
#define ENOSYS   38     /* function not implemented */
#define EOVERFLOW 75    /* value too large (a 32-bit lseek past 2 GiB) */

#endif
