#include <windows.h>
#include "utf.h"
#include "mem.h"

wchar_t *utf8_to_wide(const char *s, size_t n)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
    wchar_t *w = mem_alloc(((size_t)len + 1) * sizeof(wchar_t));

    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w, len);
    return w;
}

void wide_to_utf8(const wchar_t *w, size_t n, sb_t *out)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL);

    sb_reserve(out, (size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out->data + out->len, len, NULL, NULL);
    out->len += (size_t)len;
    out->data[out->len] = 0;
}
