/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
/*
 * string.h — the C library string/memory functions the kernel needs.
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
int    strncmp(const char *a, const char *b, size_t n);
int    strncasecmp(const char *a, const char *b, size_t n);
size_t strnlen(const char *s, size_t max);
char  *strncpy(char *dst, const char *src, size_t n);  /* pads with NULs; no NUL if src fills n */
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
size_t strlcpy(char *dst, const char *src, size_t size);   /* always ends with a NUL; returns strlen(src) */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strnstr(const char *h, const char *n, size_t len);
void  *memchr(const void *s, int c, size_t n);

#endif
