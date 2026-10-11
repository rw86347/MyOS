/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Rodger Wilson */
#ifndef LINUX_STRING_H
#define LINUX_STRING_H

/* The kernel's string.h (memcpy, strncpy, strchr, ...) plus what only Linux has. */
#include <linux/types.h>
#include <string.h>

ssize_t strscpy(char *dst, const char *src, size_t size);
#define strscpy_pad(d, s, n)  ({ memset(d, 0, n); strscpy(d, s, n); })
void  *memchr_inv(const void *s, int c, size_t n);
#define memzero_explicit(p, n) do { memset(p, 0, n); barrier(); } while (0)
int    sysfs_streq(const char *a, const char *b);
int    match_string(const char *const *array, size_t n, const char *s);
#define sysfs_match_string(a, s) match_string(a, sizeof(a) / sizeof((a)[0]), s)
static inline bool str_has_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
#define str_on_off(v)           ((v) ? "on" : "off")
#define str_enabled_disabled(v) ((v) ? "enabled" : "disabled")
#define str_yes_no(v)           ((v) ? "yes" : "no")

#endif
