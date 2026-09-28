#pragma once
/*
 * Just enough Win32 to build Silicord's portable core on Linux, where the
 * tests and the fuzzer run under AddressSanitizer and UBSan. Compiled with
 * -fshort-wchar so wchar_t is UTF-16 as on Windows.
 */
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef long LONG;
typedef long long LONG64;
typedef int BOOL;
typedef void *HANDLE;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned short USHORT;
typedef wchar_t WCHAR;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define CP_UTF8 65001
#define HEAP_ZERO_MEMORY 8
#define ERROR_NOT_ENOUGH_MEMORY 8
#define STD_OUTPUT_HANDLE ((DWORD)-11)

static inline HANDLE GetProcessHeap(void)
{
    return (HANDLE)1;
}

static inline void *HeapAlloc(HANDLE heap, DWORD flags, size_t n)
{
    (void)heap, (void)flags;
    return calloc(1, n ? n : 1);
}

static inline size_t HeapSize(HANDLE heap, DWORD flags, const void *p)
{
    (void)heap, (void)flags;
    return malloc_usable_size((void *)p);
}

static inline void *HeapReAlloc(HANDLE heap, DWORD flags, void *p, size_t n)
{
    size_t old = malloc_usable_size(p);
    void *q = realloc(p, n ? n : 1);

    (void)heap, (void)flags;
    if (q && n > old)
        memset((char *)q + old, 0, n - old);
    return q;
}

static inline BOOL HeapFree(HANDLE heap, DWORD flags, void *p)
{
    (void)heap, (void)flags;
    free(p);
    return TRUE;
}

static inline LONG64 InterlockedAdd64(volatile LONG64 *p, LONG64 v)
{
    return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}

static inline LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 x, LONG64 cmp)
{
    __atomic_compare_exchange_n(p, &cmp, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

static inline void SecureZeroMemory(void *p, size_t n)
{
    volatile char *c = p;

    while (n--)
        *c++ = 0;
}

static inline void ExitProcess(UINT code)
{
    exit((int)code);
}

static inline HANDLE GetStdHandle(DWORD which)
{
    (void)which;
    return (HANDLE)1;
}

static inline BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped)
{
    (void)h, (void)overlapped;
    *written = (DWORD)write(1, buf, n);
    return TRUE;
}

static inline int lstrlenA(const char *s)
{
    return s ? (int)strlen(s) : 0;
}

static inline int lstrcmpA(const char *a, const char *b)
{
    int r = strcmp(a, b);

    return r < 0 ? -1 : r > 0;
}

static inline char *lstrcpynA(char *dst, const char *src, int n)
{
    int i = 0;

    if (n <= 0)
        return dst;
    for (; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

/* The formats the core uses (%s, %d, %u, %.Ns) mean the same to vsprintf. */
static inline int wsprintfA(char *dst, const char *format, ...)
{
    va_list ap;
    int r;

    va_start(ap, format);
    r = vsprintf(dst, format, ap);
    va_end(ap);
    return r;
}

int MultiByteToWideChar(UINT codepage, DWORD flags, const char *s, int n, WCHAR *w, int wn);
int WideCharToMultiByte(UINT codepage, DWORD flags, const WCHAR *w, int n, char *s, int sn, const char *def, BOOL *used);
