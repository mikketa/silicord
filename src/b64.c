#include "b64.h"

static void encode(const unsigned char *d, size_t n, sb_t *out, const char *abc, int pad)
{
    size_t i;

    sb_reserve(out, (n + 2) / 3 * 4);
    for (i = 0; i + 2 < n; i += 3) {
        char q[4] = {abc[d[i] >> 2], abc[((d[i] & 3) << 4) | (d[i + 1] >> 4)],
                     abc[((d[i + 1] & 15) << 2) | (d[i + 2] >> 6)], abc[d[i + 2] & 63]};
        sb_addn(out, q, 4);
    }
    if (n - i == 1) {
        char q[2] = {abc[d[i] >> 2], abc[(d[i] & 3) << 4]};
        sb_addn(out, q, 2);
        if (pad)
            sb_addn(out, "==", 2);
    } else if (n - i == 2) {
        char q[3] = {abc[d[i] >> 2], abc[((d[i] & 3) << 4) | (d[i + 1] >> 4)], abc[(d[i + 1] & 15) << 2]};
        sb_addn(out, q, 3);
        if (pad)
            sb_addn(out, "=", 1);
    }
}

void b64_encode(const unsigned char *data, size_t n, sb_t *out)
{
    encode(data, n, out, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", 1);
}

void b64url_encode(const unsigned char *data, size_t n, sb_t *out)
{
    encode(data, n, out, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 0);
}

static int value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int b64_decode(const char *s, size_t n, sb_t *out)
{
    unsigned acc = 0;
    int bits = 0;

    while (n && s[n - 1] == '=')
        n--;
    for (size_t i = 0; i < n; i++) {
        int v = value(s[i]);
        if (v < 0)
            return 0;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            char b;
            bits -= 8;
            b = (char)((acc >> bits) & 0xFF);
            sb_addn(out, &b, 1);
        }
    }
    return 1;
}
