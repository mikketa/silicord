/* Emoji names and :shortcode: expansion. */
#include <windows.h>
#include "test.h"
#include "emoji.h"

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
    finish();
}
