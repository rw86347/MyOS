#ifndef LINUX_ERRNO_H
#define LINUX_ERRNO_H

/* Linux's error numbers: the ones programs see (errno.h), and the few that
   only kernel code uses. */
#include <errno.h>

#ifndef ERESTARTSYS
#define ERESTARTSYS   512           /* interrupted: the syscall restarts */
#endif
#define ENOTSUPP      524           /* (Linux-internal; programs see EOPNOTSUPP) */
#ifndef EOPNOTSUPP
#define EOPNOTSUPP    95
#endif
#ifndef EDOM
#define EDOM 33
#endif
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef ENOLINK
#define ENOLINK 67
#endif
#ifndef EPROTO
#define EPROTO 71
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN 112
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 113
#endif
#ifndef ECANCELED
#define ECANCELED 125
#endif
#ifndef ENOKEY
#define ENOKEY 126
#endif
#ifndef EKEYREJECTED
#define EKEYREJECTED 129
#endif
#ifndef ENOTUNIQ
#define ENOTUNIQ 76
#endif
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#ifndef ENOMEDIUM
#define ENOMEDIUM 123
#endif
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#ifndef ERFKILL
#define ERFKILL       132           /* radio switched off */
#endif

#endif
