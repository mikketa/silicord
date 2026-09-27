/* Minimal runtime: the compiler may emit calls to these even without a CRT. */
#include <stddef.h>
#include <intrin.h>

#pragma function(memset, memcpy, memmove)

/* Tells the linker floating point is used; normally defined by the CRT. */
int _fltused = 0;

void *memset(void *dst, int c, size_t n)
{
    __stosb((unsigned char *)dst, (unsigned char)c, n);
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    __movsb((unsigned char *)dst, (const unsigned char *)src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    volatile unsigned char *d = dst;
    const unsigned char *s = src;

    if (d <= s || d >= s + n) {
        __movsb((unsigned char *)dst, s, n);
    } else {
        while (n--)
            d[n] = s[n];
    }
    return dst;
}
