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

void entry(void)
{
    test_inline();
    test_links();
    test_blocks();
    md_free(&g_doc);
    finish();
}
