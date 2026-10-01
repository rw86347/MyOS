/*
 * string.c — the C memory/string functions that aren't speed-critical.
 */
#include <string.h>

/* memcpy, memset and memmove are hot and CPU-specific: see
   arch/<cpu>/memcpy.c. */

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}
