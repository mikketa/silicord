#pragma once
/* Small string helpers for the portable core: bytes as they are, no locale. */

/* ASCII only: other bytes, UTF-8 included, are left as they are. */
static __inline int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Whether two NUL-terminated strings hold the same bytes. */
static __inline int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
