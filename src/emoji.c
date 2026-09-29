#include "emoji.h"
#include "md.h"
#include "str.h"

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
    /* Discord's other names, sorted: a scan is enough for a few hundred. */
    {
        const char *p = k_emoji_aliases;
        for (int i = 0; i < k_nemoji_aliases; i++) {
            size_t k = 0;
            while (p[k])
                k++;
            if (k == n) {
                size_t j = 0;
                while (j < n && p[j] == name[j])
                    j++;
                if (j == n)
                    return p + k + 1;
            }
            p += k + 1;
            while (*p)
                p++;
            p++;
        }
    }
    return NULL;
}

static int ends_with(const char *s, size_t n, const char *tail, size_t *cut)
{
    size_t k = 0;

    while (tail[k])
        k++;
    if (n <= k)
        return 0;
    for (size_t i = 0; i < k; i++)
        if (s[n - k + i] != tail[i])
            return 0;
    *cut = n - k;
    return 1;
}

/* Byte length of the UTF-8 character at s. */
static size_t utf8_len(unsigned char c)
{
    return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

/* Appends the emoji called `name`, skin tone variants included ("thumbsup_tone2", "wave_medium_skin_tone"). */
static int emoji_append(const char *name, size_t n, sb_t *out)
{
    /* Longest first: "_medium_light_skin_tone" also ends with "_light_skin_tone". */
    static const struct {
        const char *tail;
        int tone;
    } tails[] = {{"_medium_light_skin_tone", 1}, {"_medium_dark_skin_tone", 3}, {"_medium_skin_tone", 2},
                 {"_light_skin_tone", 0},        {"_dark_skin_tone", 4},        {"_tone1", 0},
                 {"_tone2", 1},                  {"_tone3", 2},                 {"_tone4", 3},
                 {"_tone5", 4}};
    const char *e = emoji_by_name(name, n);
    size_t base_n = 0;
    int tone = -1;

    if (e) {
        sb_add(out, e);
        return 1;
    }
    for (int t = 0; t < (int)(sizeof tails / sizeof *tails) && tone < 0; t++)
        if (ends_with(name, n, tails[t].tail, &base_n))
            tone = tails[t].tone;
    if (tone < 0 || !(e = emoji_by_name(name, base_n)))
        return 0;
    /* The modifier (U+1F3FB..U+1F3FF) goes after the first character, in place of its U+FE0F. */
    {
        size_t first = utf8_len((unsigned char)e[0]), rest = first;
        char mod[4] = {'\xF0', '\x9F', '\x8F', (char)(0xBB + tone)};
        if (e[rest] == '\xEF' && e[rest + 1] == '\xB8' && e[rest + 2] == '\x8F')
            rest += 3;
        sb_addn(out, e, first);
        sb_addn(out, mod, 4);
        sb_add(out, e + rest);
    }
    return 1;
}

int emoji_matches(int i, const char *query)
{
    const char *p = k_emoji[i].names;
    int found = 0;

    if (!*query)
        return 1;
    for (const char *start = p; *p; p++) {
        size_t k = 0;
        while (query[k] && ascii_lower(p[k]) == ascii_lower(query[k]))
            k++;
        if (!query[k]) {
            if (p == start || p[-1] == ' ')
                return 2; /* a name starts with it */
            found = 1;
        }
    }
    return found;
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

    while (i < n) {
        /* Code spans and blocks (closed ones: a lone backtick is just text) stay as typed. */
        if (s[i] == '`') {
            size_t end = md_code_end(s, n, i), j = i;
            if (!end)
                while (j < n && s[j] == '`')
                    j++;
            sb_addn(out, s + i, (end ? end : j) - i);
            i = end ? end : j;
            continue;
        }
        if (s[i] == ':') {
            size_t j = i + 1;
            while (j < n && name_char(s[j]))
                j++;
            if (j < n && s[j] == ':' && j > i + 1) {
                const char *rep = custom ? custom(ctx, s + i + 1, j - i - 1) : NULL;
                if (rep)
                    sb_add(out, rep);
                if (rep || emoji_append(s + i + 1, j - i - 1, out)) {
                    i = j + 1;
                    continue;
                }
            }
        }
        sb_addn(out, s + i, 1);
        i++;
    }
}
