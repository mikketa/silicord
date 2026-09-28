/* The Win32 conversions and the assembly routine the portable core calls, for the Linux build. */
#include <windows.h>
#include "sc_asm.h"

size_t sc_strlen(const char *s)
{
    return strlen(s);
}

/* UTF-8 to UTF-16 like Windows: invalid sequences become U+FFFD; n < 0 includes the terminator. */
int MultiByteToWideChar(UINT codepage, DWORD flags, const char *s, int n, WCHAR *w, int wn)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
    int out = 0;

    (void)codepage, (void)flags;
    for (size_t i = 0; i < len;) {
        unsigned c = p[i], cp, need;
        size_t k = 1;
        if (c < 0x80)
            cp = c, need = 0;
        else if ((c & 0xE0) == 0xC0)
            cp = c & 31, need = 1;
        else if ((c & 0xF0) == 0xE0)
            cp = c & 15, need = 2;
        else if ((c & 0xF8) == 0xF0)
            cp = c & 7, need = 3;
        else
            cp = 0xFFFD, need = 0;
        for (; k <= need; k++) {
            if (i + k >= len || (p[i + k] & 0xC0) != 0x80)
                break;
            cp = cp << 6 | (p[i + k] & 63);
        }
        if (k <= need)
            cp = 0xFFFD;
        i += k;
        if (cp >= 0x10000) {
            if (w) {
                if (out + 2 > wn)
                    return 0;
                w[out] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10));
                w[out + 1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 1023));
            }
            out += 2;
        } else {
            if (w) {
                if (out + 1 > wn)
                    return 0;
                w[out] = (WCHAR)cp;
            }
            out++;
        }
    }
    return out;
}

int WideCharToMultiByte(UINT codepage, DWORD flags, const WCHAR *w, int n, char *s, int sn, const char *def, BOOL *used)
{
    size_t len = 0;
    int out = 0;

    (void)codepage, (void)flags, (void)def, (void)used;
    if (n < 0) {
        while (w[len])
            len++;
        len++;
    } else {
        len = (size_t)n;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned c = (unsigned short)w[i];
        unsigned char b[4];
        int k;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && ((unsigned short)w[i + 1] & 0xFC00) == 0xDC00)
            c = 0x10000 + ((c - 0xD800) << 10) + ((unsigned short)w[++i] - 0xDC00);
        else if (c >= 0xD800 && c < 0xE000)
            c = 0xFFFD;
        if (c < 0x80) {
            b[0] = (unsigned char)c, k = 1;
        } else if (c < 0x800) {
            b[0] = (unsigned char)(0xC0 | c >> 6), b[1] = (unsigned char)(0x80 | (c & 63)), k = 2;
        } else if (c < 0x10000) {
            b[0] = (unsigned char)(0xE0 | c >> 12), b[1] = (unsigned char)(0x80 | (c >> 6 & 63));
            b[2] = (unsigned char)(0x80 | (c & 63)), k = 3;
        } else {
            b[0] = (unsigned char)(0xF0 | c >> 18), b[1] = (unsigned char)(0x80 | (c >> 12 & 63));
            b[2] = (unsigned char)(0x80 | (c >> 6 & 63)), b[3] = (unsigned char)(0x80 | (c & 63)), k = 4;
        }
        if (s) {
            if (out + k > sn)
                return 0;
            memcpy(s + out, b, (size_t)k);
        }
        out += k;
    }
    return out;
}
