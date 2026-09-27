#include <windows.h>
#include "mem.h"

static volatile LONG64 g_used, g_peak;

static void count(LONG64 delta)
{
    LONG64 now = InterlockedAdd64(&g_used, delta), peak;

    while (now > (peak = g_peak) && InterlockedCompareExchange64(&g_peak, now, peak) != peak)
        ;
}

static void *check(void *p)
{
    if (!p)
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
    count((LONG64)HeapSize(GetProcessHeap(), 0, p));
    return p;
}

void *mem_alloc(size_t n)
{
    return check(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n));
}

void *mem_realloc(void *p, size_t n)
{
    if (!p)
        return mem_alloc(n);
    count(-(LONG64)HeapSize(GetProcessHeap(), 0, p));
    return check(HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n));
}

void mem_free(void *p)
{
    if (!p)
        return;
    count(-(LONG64)HeapSize(GetProcessHeap(), 0, p));
    HeapFree(GetProcessHeap(), 0, p);
}

size_t mem_used(void)
{
    return (size_t)g_used;
}

size_t mem_peak(void)
{
    return (size_t)g_peak;
}
