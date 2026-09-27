#include <windows.h>
#include "mem.h"

static void *check(void *p)
{
    if (!p)
        ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
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
    return check(HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n));
}

void mem_free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}
