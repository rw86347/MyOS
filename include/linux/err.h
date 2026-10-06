#ifndef LINUX_ERR_H
#define LINUX_ERR_H

/* A pointer or an error in one return value: the top 4095 addresses are
   never valid, so -errno cast to a pointer says "failed". */
#include <linux/compiler.h>
#include <linux/errno.h>

#define MAX_ERRNO       4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long error) { return (void *)error; }
static inline long PTR_ERR(const void *ptr) { return (long)ptr; }
static inline int IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline int IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR_VALUE((unsigned long)ptr); }
static inline void *ERR_CAST(const void *ptr) { return (void *)ptr; }
static inline int PTR_ERR_OR_ZERO(const void *ptr) { return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0; }

#endif
