/* Minimal runtime: the compiler may emit calls to these even without a CRT. */
#include <stddef.h>
#include <intrin.h>
#include <emmintrin.h>

#pragma function(memset, memcpy, memmove)

/* Tells the linker floating point is used; normally defined by the CRT. */
int _fltused = 0;

/*
 * Copies of at most 32 bytes, most of the calls (strings, rows of 4x4 and 16x16
 * blocks), without the startup cost of rep movsb: two loads that may overlap
 * cover the bytes, and both happen before any store, so the ranges may overlap too.
 */
static __forceinline void copy_short(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= 16) {
        __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)(s + n - 16));
        _mm_storeu_si128((__m128i *)d, a);
        _mm_storeu_si128((__m128i *)(d + n - 16), b);
    } else if (n >= 8) {
        unsigned __int64 a = *(const unsigned __int64 __unaligned *)s, b = *(const unsigned __int64 __unaligned *)(s + n - 8);
        *(unsigned __int64 __unaligned *)d = a;
        *(unsigned __int64 __unaligned *)(d + n - 8) = b;
    } else if (n >= 4) {
        unsigned a = *(const unsigned __unaligned *)s, b = *(const unsigned __unaligned *)(s + n - 4);
        *(unsigned __unaligned *)d = a;
        *(unsigned __unaligned *)(d + n - 4) = b;
    } else if (n) {
        unsigned char a = s[0], b = s[n / 2], c = s[n - 1];
        d[0] = a;
        d[n / 2] = b;
        d[n - 1] = c;
    }
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    /* Short fills as copy_short() does them, longer ones with rep stosb. */
    if (n >= 16 && n <= 32) {
        __m128i v = _mm_set1_epi8((char)c);
        _mm_storeu_si128((__m128i *)d, v);
        _mm_storeu_si128((__m128i *)(d + n - 16), v);
    } else if (n >= 8 && n < 16) {
        unsigned __int64 v = (unsigned char)c * 0x0101010101010101ull;
        *(unsigned __int64 __unaligned *)d = v;
        *(unsigned __int64 __unaligned *)(d + n - 8) = v;
    } else if (n >= 4 && n < 8) {
        unsigned v = (unsigned char)c * 0x01010101u;
        *(unsigned __unaligned *)d = v;
        *(unsigned __unaligned *)(d + n - 4) = v;
    } else if (n < 4) {
        if (n) {
            d[0] = (unsigned char)c;
            d[n / 2] = (unsigned char)c;
            d[n - 1] = (unsigned char)c;
        }
    } else {
        __stosb(d, (unsigned char)c, n);
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    if (n <= 32)
        copy_short(dst, src, n);
    else
        __movsb((unsigned char *)dst, (const unsigned char *)src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    volatile unsigned char *d = dst;
    const unsigned char *s = src;

    if (n <= 32) {
        copy_short(dst, s, n);
    } else if (d <= s || d >= s + n) {
        __movsb((unsigned char *)dst, s, n);
    } else {
        while (n--)
            d[n] = s[n];
    }
    return dst;
}
