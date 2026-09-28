/* Markdown parser tests. */
#include <windows.h>
#include "test.h"
#include "md.h"

static md_doc_t g_doc;

static void parse(const char *s)
{
    md_free(&g_doc);
    md_parse(s, sc_strlen(s), &g_doc);
}

/* Compares a UTF-16 range with an ASCII/UTF-8 string. */
static int wide_eq(const wchar_t *w, int n, const char *s)
{
    wchar_t buf[256];
    int m = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 256) - 1;

    if (m != n)
        return 0;
    for (int i = 0; i < n; i++)
        if (w[i] != buf[i])
            return 0;
    return 1;
}

static int text_is(const char *s)
{
    return wide_eq(g_doc.text, g_doc.len, s);
}

/* Flags of the span covering `sub` (first occurrence), 0 if unstyled. */
static unsigned flags_of(const char *sub)
{
    wchar_t buf[64];
    int m = MultiByteToWideChar(CP_UTF8, 0, sub, -1, buf, 64) - 1;

    for (int i = 0; i + m <= g_doc.len; i++) {
        int k = 0;
        while (k < m && g_doc.text[i + k] == buf[k])
            k++;
        if (k < m)
            continue;
        for (int sp = 0; sp < g_doc.nspans; sp++)
            if (g_doc.spans[sp].start <= i && i + m <= g_doc.spans[sp].start + g_doc.spans[sp].len)
                return g_doc.spans[sp].flags;
        return 0;
    }
    return 0xFFFFFFFFu;
}

static void test_inline(void)
{
    parse("a **b** c");
    check(text_is("a b c") && flags_of("b") == MD_BOLD && flags_of("a") == 0, "bold");
    parse("*i* _j_ __u__ ~~s~~");
    check(text_is("i j u s") && flags_of("i") == MD_ITALIC && flags_of("j") == MD_ITALIC &&
          flags_of("u") == MD_UNDERLINE && flags_of("s") == MD_STRIKE, "italic, underline, strike");
    parse("**bold *both***");
    check(flags_of("both") == (MD_BOLD | MD_ITALIC), "nested styles");
    parse("`a*b*` ok");
    check(text_is("a*b* ok") && flags_of("a*b*") == MD_CODE, "code spans are literal");
    parse("||secret||");
    check(text_is("secret") && flags_of("secret") == MD_SPOILER, "spoiler");
    parse("\\*not\\* snake_case_name");
    check(text_is("*not* snake_case_name"), "escapes and snake_case stay literal");
    parse(MD_MENTION_OPEN "@bob" MD_MENTION_CLOSE " hi");
    check(text_is("@bob hi") && flags_of("@bob") == MD_MENTION, "mentions");
}

static void test_links(void)
{
    parse("see https://x.com/a_b. then");
    check(text_is("see https://x.com/a_b. then") && flags_of("https://x.com/a_b") == MD_LINK &&
          flags_of(". then") == 0 && lstrcmpA(md_link(&g_doc, 0), "https://x.com/a_b") == 0,
          "bare link without trailing punctuation");
    parse("[the site](https://e.com/p) and <https://q.org>");
    check(text_is("the site and https://q.org") && flags_of("the site") == MD_LINK && g_doc.nlinks == 2 &&
          lstrcmpA(md_link(&g_doc, 0), "https://e.com/p") == 0 && lstrcmpA(md_link(&g_doc, 1), "https://q.org") == 0,
          "masked link and <link>");
    parse("[x](javascript:alert)");
    check(text_is("[x](javascript:alert)") && g_doc.nlinks == 0, "only http(s) links");
}

static void test_emoji(void)
{
    parse("hi " MD_EMOJI_OPEN "a55:party" MD_EMOJI_CLOSE "!");
    check(text_is("hi \xEF\xBF\xBC!") && flags_of("\xEF\xBF\xBC") == MD_EMOJI && g_doc.nlinks == 1 &&
              lstrcmpA(md_link(&g_doc, 0), "a55") == 0 && !g_doc.jumbo,
          "custom emoji become one object character");
    parse(MD_EMOJI_OPEN "1:a" MD_EMOJI_CLOSE " \xF0\x9F\x98\x80");
    check(g_doc.jumbo, "emoji-only messages are jumbo");
    parse("ok \xF0\x9F\x98\x80");
    check(!g_doc.jumbo, "text with emoji is not jumbo");
    parse("fixed " MD_EDITED_MARK);
    check(text_is("fixed (edited)") && flags_of("(edited)") == MD_EDITED, "edited label");
    parse("\xF0\x9F\x98\x80 " MD_EDITED_MARK);
    check(g_doc.jumbo, "the edited label does not break jumbo emoji");
}

static void test_blocks(void)
{
    parse("> q1\n> q2\nplain\n```js\ncode *x*\n```\n# Title\n- a\n- b");
    check(g_doc.nblocks == 5, "block count");
    if (g_doc.nblocks == 5) {
        md_block_t *b = g_doc.blocks;
        check(b[0].kind == MD_QUOTE && wide_eq(g_doc.text + b[0].start, b[0].len, "q1\nq2"), "quote lines merge");
        check(b[1].kind == MD_PARA && wide_eq(g_doc.text + b[1].start, b[1].len, "plain"), "paragraph");
        check(b[2].kind == MD_CODEBLOCK && wide_eq(g_doc.text + b[2].start, b[2].len, "code *x*"),
              "code block drops the language and stays literal");
        check(b[3].kind == MD_H1 && wide_eq(g_doc.text + b[3].start, b[3].len, "Title"), "heading");
        check(b[4].kind == MD_LIST && wide_eq(g_doc.text + b[4].start, b[4].len, "\xE2\x80\xA2  a\n\xE2\x80\xA2  b"),
              "list items get bullets");
    }
    parse(">>> all\nof this");
    check(g_doc.nblocks == 1 && g_doc.blocks[0].kind == MD_QUOTE, "block quote takes the rest");
}

/* Cases where Discord's rules differ from a naive reading. */
static void test_discord_rules(void)
{
    parse("2 ** 3 = 8");
    check(text_is("2 ** 3 = 8"), "a lone ** stays text");
    parse("**a");
    check(text_is("**a"), "an unclosed ** stays text");
    parse("*a **b** c*");
    check(text_is("a b c") && flags_of("a ") == MD_ITALIC && flags_of("b") == (MD_ITALIC | MD_BOLD), "italic around bold");
    parse("*one\ntwo*");
    check(text_is("one\ntwo") && flags_of("one") == MD_ITALIC && flags_of("two") == MD_ITALIC, "italic across lines");
    parse("a ```x``` b ``c`d``");
    check(text_is("a x b c`d") && flags_of("x") == MD_CODE && flags_of("c`d") == MD_CODE, "code spans of any length");
    parse("\\. \\! \\= \\\xC3\xA9 \\a");
    check(text_is(". ! = \xC3\xA9 \\a"), "a backslash escapes any punctuation or non-ASCII character, not letters");
    parse("[docs](<https://x.com>)");
    check(text_is("docs") && flags_of("docs") == MD_LINK && lstrcmpA(md_link(&g_doc, 0), "https://x.com") == 0,
          "masked link with an angle-bracketed URL");
    parse("[1] see [here](https://x.com)");
    check(text_is("[1] see here") && flags_of("here") == MD_LINK && flags_of("[1] see ") == 0,
          "masked link text starts at the nearest bracket");
    parse("[w](https://en.wikipedia.org/wiki/Foo_(bar)) end");
    check(text_is("w end") && lstrcmpA(md_link(&g_doc, 0), "https://en.wikipedia.org/wiki/Foo_(bar)") == 0,
          "masked link URL with parentheses");
}

static void test_nested_blocks(void)
{
    parse(">>> # Title\n- a\n```\ncode\n```\ntext");
    check(g_doc.nblocks == 4 && g_doc.blocks[0].kind == MD_H1 && g_doc.blocks[0].quoted && g_doc.blocks[1].kind == MD_LIST &&
              g_doc.blocks[1].quoted && g_doc.blocks[2].kind == MD_CODEBLOCK && g_doc.blocks[3].kind == MD_QUOTE,
          "blocks inside a block quote (forwarded messages)");
    parse("> # T\nafter");
    check(g_doc.nblocks == 2 && g_doc.blocks[0].kind == MD_H1 && g_doc.blocks[0].quoted && g_doc.blocks[1].kind == MD_PARA &&
              !g_doc.blocks[1].quoted,
          "a heading in a one-line quote");
    parse(">no space");
    check(g_doc.nblocks == 1 && g_doc.blocks[0].kind == MD_PARA && text_is(">no space"), "a quote needs a space");
    parse("1. one\n2. two\n  - sub");
    check(g_doc.nblocks == 1 && g_doc.blocks[0].kind == MD_LIST &&
              text_is("1.  one\n2.  two\n\xE2\x80\x83\xE2\x80\x83\xE2\x97\xA6  sub"),
          "numbered and nested lists");
    check(md_code_end("`<@1>` x", 8, 0) == 6 && md_code_end("```a```", 7, 0) == 7 && md_code_end("`open", 5, 0) == 0,
          "code regions for the formatters");
}

void entry(void)
{
    test_inline();
    test_discord_rules();
    test_nested_blocks();
    test_links();
    test_emoji();
    test_blocks();
    md_free(&g_doc);
    finish();
}
