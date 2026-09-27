/* Minimal runtime: the compiler may emit calls to these even without a CRT. */
#include <stddef.h>
#include <intrin.h>

#pragma function(memset, memcpy)

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
