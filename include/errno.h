#ifndef ERRNO_H
#define ERRNO_H

/* Linux errno values (asm-generic/errno-base.h). Syscalls return -ERRNO
   on failure; the C library turns that into -1 and sets errno. They must
   match Linux, because compiled programs compare against these numbers. */
#define ENOENT    2     /* no such file or directory */
#define EPERM     1     /* not allowed */
#define ESRCH     3     /* no such process */
#define EINTR     4     /* interrupted by a signal */
#define ENOEXEC   8     /* not an executable we can run */
#define EBADF     9     /* bad file descriptor */
#define ECHILD   10     /* no child processes */
#define EAGAIN   11     /* try again */
#define ENOMEM   12     /* out of memory */
#define EACCES   13     /* permission denied */
#define EFAULT   14     /* bad address */
#define ENOTDIR  20     /* a path component is not a directory */
#define EISDIR   21     /* is a directory */
#define ENODEV   19     /* no such device */
#define EEXIST   17     /* file exists */
#define EXDEV    18     /* cross-device link */
#define EINVAL   22     /* invalid argument */
#define EMFILE   24     /* too many open files */
#define ENOTTY   25     /* not a terminal */
#define EFBIG    27     /* file too large */
#define ENOSPC   28     /* no space left (out of RAM) */
#define ESPIPE   29     /* can't seek (e.g. the console) */
#define EMLINK   31     /* too many links */
#define EROFS    30     /* read-only file system */
#define EPIPE    32     /* reading end of the pipe is closed */
#define ERANGE   34     /* result too large (e.g. getcwd buffer) */
#define E2BIG     7     /* argument list too long */
#define ENAMETOOLONG 36 /* file name too long */
#define ENOSYS   38     /* function not implemented */
#define ENOTEMPTY 39    /* directory not empty */
#define ELOOP    40     /* too many symbolic links */
#define EOVERFLOW 75    /* value too large (a 32-bit lseek past 2 GiB) */
#define ENOTSOCK 88     /* not a socket */

#endif
