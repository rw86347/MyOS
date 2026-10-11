/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef ERRNO_H
#define ERRNO_H

/* Linux errno values (asm-generic/errno-base.h). Syscalls return -ERRNO
   on failure; the C library turns that into -1 and sets errno. They must
   match Linux, because compiled programs compare against these numbers. */
#define ENOENT    2     /* no such file or directory */
#define EPERM     1     /* not allowed */
#define ESRCH     3     /* no such process */
#define EINTR     4     /* interrupted by a signal */
#define EIO       5     /* the disk said no (read/write error) */
#define ENXIO     6     /* no such device (a node with no driver) */
#define ENOEXEC   8     /* not an executable we can run */
#define EBADF     9     /* bad file descriptor */
#define ECHILD   10     /* no child processes */
#define EAGAIN   11     /* try again */
#define ENOMEM   12     /* out of memory */
#define EACCES   13     /* permission denied */
#define EFAULT   14     /* bad address */
#define ENOTBLK  15     /* mount needs a block device */
#define EBUSY    16     /* in use ("/" can't be removed) */
#define ENOTDIR  20     /* a path component is not a directory */
#define EISDIR   21     /* is a directory */
#define ENODEV   19     /* no such device */
#define EEXIST   17     /* file exists */
#define EXDEV    18     /* cross-device link */
#define EINVAL   22     /* invalid argument */
#define ENFILE   23     /* too many open files in the system */
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
#define EDESTADDRREQ 89 /* no destination address */
#define EMSGSIZE 90     /* message too long */
#define EPROTOTYPE 91   /* wrong protocol for this socket type */
#define ENOPROTOOPT 92  /* that socket option does not exist */
#define EPROTONOSUPPORT 93 /* protocol not supported */
#define ESOCKTNOSUPPORT 94 /* socket type not supported */
#define EBADFD    77    /* file descriptor in bad state (a sound stream not set up) */
#define EOPNOTSUPP 95   /* not supported on this socket */
#define EAFNOSUPPORT 97 /* address family not supported */
#define EADDRINUSE 98   /* address already in use */
#define EADDRNOTAVAIL 99 /* that address is not ours */
#define ENOBUFS 105    /* no room in a queue (a netlink socket's) */
#define EALREADY   114  /* a connect is already under way */
#define EINPROGRESS 115 /* non-blocking connect started */
#define ENETDOWN 100    /* the network device is not up */
#define ENETUNREACH 101 /* no route to that network */
#define EISCONN  106    /* already connected */
#define ECONNRESET 104  /* the other end reset the connection */
#define ENOTCONN 107    /* not connected */
#define ETIMEDOUT 110   /* timed out */
#define ECONNREFUSED 111 /* nothing was listening */
#define EDEADLK  35     /* a lock wait would deadlock */
#define ENOLCK   37     /* no locks left */
#define ENOMSG   42     /* no message of the wanted type */
#define EIDRM    43     /* removed while we waited (System V IPC) */

#endif
