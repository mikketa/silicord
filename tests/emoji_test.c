/* Emoji names, :shortcode: expansion, and Discord's images of them. */
#include <windows.h>
#include "test.h"
#include "emoji.h"
#include "twemoji.h"

static const char *custom(void *ctx, const char *name, size_t n)
{
    (void)ctx;
    return n == 3 && name[0] == 'p' && name[1] == 'o' && name[2] == 'g' ? "<:pog:70>" : NULL;
}

static int expands_to(const char *in, const char *want)
{
    sb_t out = {0};
    int ok;

    emoji_expand(in, sc_strlen(in), &out, custom, NULL);
    ok = str_eq(&out, want);
    sb_free(&out);
    return ok;
}

/* Whether s (UTF-16) starts with an emoji of `units` units whose SVG is `asset`. */
static int twemoji_is(const wchar_t *s, int mode, int units, unsigned long long asset)
{
    int index = -1, n = twemoji_match(s, lstrlenW(s), mode, &index);

    return n == units && index >= 0 && k_twemoji_asset[index] == asset;
}

void entry(void)
{
    const char *e;
    char name[32];
    int total = 0;

    e = emoji_by_name("thumbsup", 8);
    check(e && lstrcmpA(e, "\xF0\x9F\x91\x8D") == 0, "thumbsup");
    e = emoji_by_name("+1", 2);
    check(e && lstrcmpA(e, "\xF0\x9F\x91\x8D") == 0, "aliases work too");
    check(!emoji_by_name("notanemoji", 10), "unknown names");
    for (int c = 0; c < EMOJI_CATEGORIES; c++)
        total += k_emoji_categories[c].end - k_emoji_categories[c].first;
    check(total == k_nemoji && k_nemoji > 1500 && lstrcmpA(k_emoji_categories[0].name, "People") == 0,
          "categories cover the table");
    emoji_main_name(0, name, sizeof name);
    check(lstrcmpA(name, "grinning") == 0, "main name");
    check(emoji_matches(0, "GRIN") == 2 && emoji_matches(0, "nning") == 1 && !emoji_matches(0, "zzz") && emoji_matches(0, ""),
          "search: prefix, substring, none");

    check(expands_to("hi :smile: :pog: :nope:", "hi \xF0\x9F\x98\x84 <:pog:70> :nope:"), "unicode and custom names");
    check(expands_to("`:smile:` ```\n:smile:\n``` :smile:", "`:smile:` ```\n:smile:\n``` \xF0\x9F\x98\x84"),
          "code is left alone");
    check(expands_to("time 10:30:00", "time 10:30:00"), "colons in text stay");
    check(expands_to("``a :smile: b`` :smile:", "``a :smile: b`` \xF0\x9F\x98\x84"), "double backtick code stays");
    check(expands_to("it`s :smile:", "it`s \xF0\x9F\x98\x84"), "a lone backtick does not start code");
    check(expands_to(":slight_smile: :flag_us: :upside_down:", "\xF0\x9F\x99\x82 \xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 \xF0\x9F\x99\x83"),
          "Discord's own names");
    check(expands_to(":thumbsup_tone2: :wave_medium_dark_skin_tone:",
                     "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC \xF0\x9F\x91\x8B\xF0\x9F\x8F\xBE"),
          "skin tone variants");
    check(expands_to(":thumbsup::skin-tone-5:", "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF"), "a skin tone after the emoji");
    check(expands_to(":woman_lifting_weights_tone1:",
                     "\xF0\x9F\x8F\x8B\xF0\x9F\x8F\xBB\xE2\x80\x8D\xE2\x99\x80\xEF\xB8\x8F"),
          "the tone goes after the first character of a sequence");

    {
        char url[64];
        int sorted = 1, index = -1;
        for (int k = 1; k < k_ntwemoji; k++) { /* by code unit, strictly: what the lookup's binary search needs */
            const wchar_t *a, *b;
            int an = twemoji_text(k - 1, &a), bn = twemoji_text(k, &b), j = 0;
            while (j < an && j < bn && a[j] == b[j])
                j++;
            sorted &= an > 0 && (j < an && j < bn ? a[j] < b[j] : an < bn);
        }
        check(k_ntwemoji > 3500 && sorted, "twemoji table");
        {
            int found = 1;
            for (int k = 0; k < k_ntwemoji; k++) { /* each emoji, looked up, is itself */
                const wchar_t *s;
                int n = twemoji_text(k, &s);
                found &= twemoji_match(s, n, TWEMOJI_ANY, &index) == n && index == k;
            }
            check(found, "every emoji of the table is found");
        }
        check(twemoji_is(L"\xD83D\xDC4D!", TWEMOJI_COLOR, 2, 0xa4faf6864a96a042ull), "thumbs up");
        check(twemoji_is(L"\xD83D\xDC4D\xD83C\xDFFD", TWEMOJI_COLOR, 4, 0x9c107220ede7ac6cull), "a skin tone");
        check(twemoji_is(L"\x2764\xFE0F a", TWEMOJI_COLOR, 2, 0x8838f014a5d2ce59ull), "U+FE0F goes with it");
        check(twemoji_is(L"\x2764", TWEMOJI_ANY, 1, 0x8838f014a5d2ce59ull) && !twemoji_match(L"\x2764", 1, TWEMOJI_COLOR, &index),
              "a bare heart: an emoji in messages, text elsewhere");
        check(twemoji_is(L"\xD83C\xDDEB\xD83C\xDDF7\xD83C\xDDEB", TWEMOJI_COLOR, 4, 0xfc3646af3ee101f7ull), "a flag is two letters");
        check(twemoji_is(L"\xD83C\xDFF3\xFE0F\x200D\xD83C\xDF08", TWEMOJI_COLOR, 6, 0x43c4377ffbafe2daull),
              "ZWJ sequence with U+FE0F inside");
        check(twemoji_is(L"\xD83D\xDC41\xFE0F\x200D\xD83D\xDDE8\xFE0F", TWEMOJI_COLOR, 7, 0x339a006319d83b7full),
              "ZWJ sequence Twemoji names without U+FE0F");
        check(twemoji_is(L"\xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67", TWEMOJI_COLOR, 8, 0xdfb3740e7b2d0a65ull),
              "a family");
        check(twemoji_match(L"\xD83D\xDC68\x200D\xD83D\xDE00", 5, TWEMOJI_COLOR, &index) == 2,
              "a sequence without an image: its first emoji");
        check(twemoji_is(L"#\xFE0F\x20E3", TWEMOJI_COLOR, 3, 0xf8286e00b2b57f0dull), "a keycap");
        check(!twemoji_match(L"#1 a", 4, TWEMOJI_ANY, &index) && !twemoji_match(L"\x2122", 1, TWEMOJI_COLOR, &index),
              "plain text");
        check(twemoji_match(L"\xD83C\xDFF4\xDB40\xDC67\xDB40\xDC62\xDB40\xDC65\xDB40\xDC6E\xDB40\xDC67\xDB40\xDC7F", 14,
                            TWEMOJI_COLOR, &index) == 14,
              "a tag sequence (England)");
        twemoji_match(L"\xD83D\xDC4D", 2, TWEMOJI_COLOR, &index);
        twemoji_url(index, url);
        check(lstrcmpA(url, "https://discord.com/assets/a4faf6864a96a042.svg") == 0, "its URL");
    }
    finish();
}
