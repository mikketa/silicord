#include "search.h"
#include "msg.h"

#define DISCORD_EPOCH 1420070400000ll

static const char *const k_keys[] = {"from", "mentions", "has", "in", "before", "after", "during", "pinned"};

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* The filter named by s[0..n) ("from", "has"...), or -1. */
static int key_of(const char *s, size_t n)
{
    for (int k = 0; k < (int)(sizeof k_keys / sizeof *k_keys); k++) {
        size_t i = 0;
        while (i < n && k_keys[k][i] && lower((unsigned char)s[i]) == k_keys[k][i])
            i++;
        if (i == n && !k_keys[k][i])
            return k;
    }
    return -1;
}

int search_parse(const char *q, search_filter_t *f, int max, sb_t *content)
{
    int count = 0;
    const char *p = q;

    while (*p) {
        const char *word, *colon = NULL;
        size_t n;
        int key;
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        word = p;
        while (*p && *p != ' ') {
            if (*p == ':' && !colon)
                colon = p;
            p++;
        }
        n = (size_t)(p - word);
        key = colon && colon + 1 < p ? key_of(word, (size_t)(colon - word)) : -1;
        if (key >= 0 && count < max) {
            const char *v = colon + 1;
            size_t k = 0;
            if (*v == '@' || *v == '#')
                v++;
            while (v < p && k < sizeof f[count].value - 1)
                f[count].value[k++] = *v++;
            f[count].value[k] = 0;
            f[count].key = key;
            count++;
        } else {
            if (content->len)
                sb_add(content, " ");
            sb_addn(content, word, n);
        }
    }
    return count;
}

unsigned long long search_day_snowflake(const char *date, int next_day)
{
    long long ms;
    int digits = 0;

    for (const char *c = date; *c; c++)
        digits += *c >= '0' && *c <= '9';
    if (digits != 8 || date[4] != '-' || date[7] != '-')
        return 0;
    ms = msg_iso_ms(date) + (next_day ? 86400000ll : 0);
    return ms > DISCORD_EPOCH ? (unsigned long long)(ms - DISCORD_EPOCH) << 22 : 0;
}
