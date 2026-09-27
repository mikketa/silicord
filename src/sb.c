#include <windows.h>
#include <string.h>
#include "sb.h"
#include "mem.h"
#include "sc_asm.h"

void sb_reserve(sb_t *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    size_t cap;

    if (need <= sb->cap)
        return;
    cap = sb->cap ? sb->cap : 64;
    while (cap < need)
        cap *= 2;
    sb->data = mem_realloc(sb->data, cap);
    sb->cap = cap;
}

void sb_addn(sb_t *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void sb_add(sb_t *sb, const char *s)
{
    sb_addn(sb, s, sc_strlen(s));
}

void sb_u64(sb_t *sb, unsigned long long v)
{
    char tmp[20];
    int i = sizeof tmp;

    do {
        tmp[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    sb_addn(sb, tmp + i, sizeof tmp - i);
}

void sb_i64(sb_t *sb, long long v)
{
    if (v < 0) {
        sb_addn(sb, "-", 1);
        sb_u64(sb, 0ull - (unsigned long long)v);
    } else {
        sb_u64(sb, (unsigned long long)v);
    }
}

void sb_json_str(sb_t *sb, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";

    sb_addn(sb, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  sb_addn(sb, "\\\"", 2); break;
        case '\\': sb_addn(sb, "\\\\", 2); break;
        case '\n': sb_addn(sb, "\\n", 2); break;
        case '\r': sb_addn(sb, "\\r", 2); break;
        case '\t': sb_addn(sb, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
                sb_addn(sb, esc, sizeof esc);
            } else {
                sb_addn(sb, (const char *)&s[i], 1);
            }
        }
    }
    sb_addn(sb, "\"", 1);
}

void sb_clear(sb_t *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = 0;
}

void sb_free(sb_t *sb)
{
    if (sb->data) {
        SecureZeroMemory(sb->data, sb->cap);
        mem_free(sb->data);
    }
    sb->data = NULL;
    sb->len = sb->cap = 0;
}
