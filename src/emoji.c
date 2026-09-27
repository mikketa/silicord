#include "emoji.h"

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+';
}

const char *emoji_by_name(const char *name, size_t n)
{
    for (int i = 0; i < k_nemoji; i++) {
        const char *p = k_emoji[i].names;
        while (*p) {
            size_t k = 0;
            while (p[k] && p[k] != ' ')
                k++;
            if (k == n) {
                size_t j = 0;
                while (j < n && p[j] == name[j])
                    j++;
                if (j == n)
                    return k_emoji[i].emoji;
            }
            p += k + (p[k] == ' ');
        }
    }
    return NULL;
}

int emoji_matches(int i, const char *query)
{
    const char *p = k_emoji[i].names;

    if (!*query)
        return 1;
    for (; *p; p++) {
        size_t k = 0;
        while (query[k] && lower(p[k]) == lower(query[k]))
            k++;
        if (!query[k])
            return 1;
    }
    return 0;
}

void emoji_main_name(int i, char *out, size_t size)
{
    size_t k = 0;
    const char *p = k_emoji[i].names;

    while (p[k] && p[k] != ' ' && k + 1 < size) {
        out[k] = p[k];
        k++;
    }
    out[k] = 0;
}

void emoji_expand(const char *s, size_t n, sb_t *out, emoji_custom_fn custom, void *ctx)
{
    size_t i = 0;
    int code = 0; /* inside `code` (1) or a ``` block (3) */

    while (i < n) {
        if (s[i] == '`') {
            int run = (i + 2 < n && s[i + 1] == '`' && s[i + 2] == '`') ? 3 : 1;
            if (!code)
                code = run;
            else if (run == code)
                code = 0;
            sb_addn(out, s + i, (size_t)run);
            i += (size_t)run;
            continue;
        }
        if (!code && s[i] == ':') {
            size_t j = i + 1;
            while (j < n && name_char(s[j]))
                j++;
            if (j < n && s[j] == ':' && j > i + 1) {
                const char *rep = custom ? custom(ctx, s + i + 1, j - i - 1) : NULL;
                if (!rep)
                    rep = emoji_by_name(s + i + 1, j - i - 1);
                if (rep) {
                    sb_add(out, rep);
                    i = j + 1;
                    continue;
                }
            }
        }
        sb_addn(out, s + i, 1);
        i++;
    }
}
