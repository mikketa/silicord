#pragma once
/* Tiny test harness shared by the no-CRT test executables. */
#include <windows.h>
#include "sb.h"
#include "sc_asm.h"

static int g_failed;

static __inline void out(const char *s)
{
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)sc_strlen(s), &w, NULL);
}

static __inline void check(int cond, const char *name)
{
    out(cond ? "ok   " : "FAIL ");
    out(name);
    out("\r\n");
    if (!cond)
        g_failed++;
}

static __inline int bytes_eq(const sb_t *sb, const char *s, size_t n)
{
    if (sb->len != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (sb->data[i] != s[i])
            return 0;
    return 1;
}

static __inline int str_eq(const sb_t *sb, const char *s)
{
    return bytes_eq(sb, s, sc_strlen(s));
}

static __inline void finish(void)
{
    out(g_failed ? "\r\nsome tests failed\r\n" : "\r\nall tests passed\r\n");
    ExitProcess((UINT)g_failed);
}
