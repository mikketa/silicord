/* Assembly string routines: sc_strlen at every alignment, and ending at a page boundary. */
#include <windows.h>
#include "test.h"

static size_t slow_strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

static void test_alignments(void)
{
    static char buf[256 + 64];
    int ok = 1;

    for (int start = 0; start < 32; start++)
        for (int len = 0; len < 200; len++) {
            char *s = buf + start;
            for (int i = 0; i < (int)sizeof buf; i++)
                buf[i] = (char)(0x80 | (i * 7)); /* no zero bytes around */
            s[len] = 0;
            if (sc_strlen(s) != (size_t)len || slow_strlen(s) != (size_t)len)
                ok = 0;
        }
    check(ok, "sc_strlen at every alignment and length up to 200");
}

/* The last byte of a readable page is the terminator; the next page is not readable. */
static void test_page_end(void)
{
    SYSTEM_INFO si;
    char *p;
    DWORD old;
    int ok = 1;

    GetSystemInfo(&si);
    p = VirtualAlloc(NULL, si.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        check(0, "allocate two pages");
        return;
    }
    VirtualProtect(p + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &old);
    for (int len = 0; len < 64; len++) {
        char *s = p + si.dwPageSize - 1 - len;
        for (int i = 0; i < len; i++)
            s[i] = 'a';
        s[len] = 0;
        if (sc_strlen(s) != (size_t)len)
            ok = 0;
    }
    check(ok, "sc_strlen never reads past the page holding the terminator");
    VirtualFree(p, 0, MEM_RELEASE);
}

void entry(void)
{
    test_alignments();
    test_page_end();
    check(sc_strlen("") == 0 && sc_strlen("silicord") == 8, "short strings");
    finish();
}
