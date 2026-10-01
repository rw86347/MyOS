/*
 * string.h — the few C library string/memory functions the kernel needs.
 * There is no libc in the kernel, so we write our own. GCC may also emit
 * calls to memcpy/memset/memmove/memcmp on its own (e.g. for struct copies),
 * so these four must always exist.
 */
#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strcasecmp(const char *a, const char *b);   /* FAT names ignore case */

#endif
