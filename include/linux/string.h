#ifndef LINUX_STRING_H
#define LINUX_STRING_H

/* The kernel's string.h (memcpy, strlen, ...) plus what Linux drivers add. */
#include <linux/types.h>
#include <string.h>

int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n);
ssize_t strscpy(char *dst, const char *src, size_t size);
#define strscpy_pad(d, s, n)  ({ memset(d, 0, n); strscpy(d, s, n); })
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strnlen(const char *s, size_t max);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strnstr(const char *h, const char *n, size_t len);
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
int    strncasecmp(const char *a, const char *b, size_t n);
void  *memchr(const void *s, int c, size_t n);
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
