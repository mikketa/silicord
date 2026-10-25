#include "twemoji.h"

#define MAX_UNITS 40

/* Code point at s[i], its length in units in *len. */
static unsigned code_at(const wchar_t *s, int n, int i, int *len)
{
    unsigned c = s[i];

    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
        *len = 2;
        return 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
    }
    *len = 1;
    return c;
}

/* Emoji_Presentation in the BMP (Unicode's emoji-data.txt): drawn as emoji without U+FE0F. */
static int presentation(unsigned c)
{
    static const unsigned short ranges[][2] = {
        {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
        {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
        {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
        {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
        {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    };

    for (int k = 0; k < (int)(sizeof ranges / sizeof ranges[0]); k++)
        if (c >= ranges[k][0] && c <= ranges[k][1])
            return 1;
    return 0;
}

static int regional(unsigned c)
{
    return c >= 0x1F1E6 && c <= 0x1F1FF;
}

int twemoji_text(int k, const wchar_t **s)
{
    int start = k ? k_twemoji_end[k - 1] : 0;

    *s = k_twemoji_text + start;
    return k_twemoji_end[k] - start;
}

/* a[0, an) against b[0, bn), as the table is sorted: by code unit, a prefix first. */
static int compare(const wchar_t *a, int an, const wchar_t *b, int bn)
{
    for (int j = 0; j < an && j < bn; j++)
        if (a[j] != b[j])
            return a[j] < b[j] ? -1 : 1;
    return an < bn ? -1 : an > bn;
}

static int find(const wchar_t *key, int len)
{
    int lo = 0, hi = k_ntwemoji - 1;

    while (lo <= hi) {
        const wchar_t *s;
        int mid = (lo + hi) / 2, n = twemoji_text(mid, &s), c = compare(s, n, key, len);
        if (!c)
            return mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int twemoji_match(const wchar_t *s, int n, int mode, int *index)
{
    wchar_t key[MAX_UNITS];
    int ends[MAX_UNITS], keys[MAX_UNITS]; /* after each code point kept: units of s read, units of key */
    int nk = 0, nb = 0, i = 0, l, cps = 0, zwj = 0, flags = 0;
    unsigned c;

    if (n <= 0)
        return 0;
    c = s[0];
    /* Most text: nothing to look up. A keycap starts with an ASCII digit, '#' or '*'. */
    if (c < 0xA9 && !((c >= '0' && c <= '9') || c == '#' || c == '*'))
        return 0;
    if (c >= 0xDC00 && c < 0xE000)
        return 0;
    c = code_at(s, n, 0, &l);
    if (c < 0xA9 || mode == TWEMOJI_COLOR) {
        unsigned next = l < n ? s[l] : 0;
        if (!(c >= 0x1F000 || (c >= 0x2000 && presentation(c)) || next == 0xFE0F || next == 0x20E3))
            return 0;
    }
    while (i < n) {
        c = code_at(s, n, i, &l);
        if (cps) {
            int more = c == 0xFE0F || c == 0x200D || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) ||
                       (c >= 0xE0020 && c <= 0xE007F) || zwj || (flags == 1 && regional(c));
            if (!more)
                break;
        }
        if (c != 0xFE0F) {
            if (nk + l > MAX_UNITS)
                break;
            for (int k = 0; k < l; k++)
                key[nk++] = s[i + k];
        }
        zwj = c == 0x200D;
        flags += regional(c);
        i += l;
        cps++;
        if (c != 0xFE0F && !zwj) {
            ends[nb] = i;
            keys[nb++] = nk;
        }
    }
    /* The longest that has an image. */
    while (nb-- > 0) {
        int k = find(key, keys[nb]);
        if (k >= 0) {
            int used = ends[nb];
            if (used < n && s[used] == 0xFE0F)
                used++;
            *index = k;
            return used;
        }
    }
    return 0;
}

void twemoji_url(int index, char *out)
{
    static const char hex[] = "0123456789abcdef", prefix[] = "https://discord.com/assets/";
    unsigned long long a = k_twemoji_asset[index];
    int n = 0;

    for (const char *p = prefix; *p; p++)
        out[n++] = *p;
    for (int k = 15; k >= 0; k--)
        out[n++] = hex[(a >> (4 * k)) & 15];
    out[n++] = '.';
    out[n++] = 's';
    out[n++] = 'v';
    out[n++] = 'g';
    out[n] = 0;
}
