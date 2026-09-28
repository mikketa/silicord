#include <windows.h>
#include "md.h"
#include "mem.h"

#define NPOS ((size_t)-1)

/* ---- Output ---- */

static void add_span(md_doc_t *d, int start, int len, unsigned flags, int link)
{
    md_span_t *last = d->nspans ? &d->spans[d->nspans - 1] : NULL;

    if (last && last->start + last->len == start && last->flags == flags && last->link == link) {
        last->len += len;
        return;
    }
    if (d->nspans == d->cap_spans) {
        d->cap_spans = d->cap_spans ? d->cap_spans * 2 : 16;
        d->spans = mem_realloc(d->spans, (size_t)d->cap_spans * sizeof *d->spans);
    }
    d->spans[d->nspans].start = start;
    d->spans[d->nspans].len = len;
    d->spans[d->nspans].flags = flags;
    d->spans[d->nspans].link = link;
    d->nspans++;
}

static void emit(md_doc_t *d, const char *s, size_t n, unsigned flags, int link)
{
    int wn;

    if (!n)
        return;
    wn = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
    if (d->len + wn + 1 > d->cap) {
        while (d->len + wn + 1 > d->cap)
            d->cap = d->cap ? d->cap * 2 : 128;
        d->text = mem_realloc(d->text, (size_t)d->cap * sizeof(wchar_t));
    }
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, d->text + d->len, wn);
    if (flags)
        add_span(d, d->len, wn, flags, link);
    d->len += wn;
    d->text[d->len] = 0;
}

static int add_link(md_doc_t *d, const char *url, size_t n)
{
    sb_addn(&d->links, url, n);
    sb_addn(&d->links, "", 1);
    return d->nlinks++;
}

const char *md_link(const md_doc_t *doc, int i)
{
    const char *p = doc->links.data;

    if (i < 0 || i >= doc->nlinks)
        return "";
    while (i--)
        p += lstrlenA(p) + 1;
    return p;
}

/* ---- Scanning helpers ---- */

static int starts(const char *s, size_t n, size_t i, const char *tok)
{
    for (size_t k = 0; tok[k]; k++)
        if (i + k >= n || s[i + k] != tok[k])
            return 0;
    return 1;
}

static size_t find(const char *s, size_t n, size_t from, const char *tok)
{
    for (size_t i = from; i < n; i++)
        if (starts(s, n, i, tok))
            return i;
    return NPOS;
}

static int is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_space(char c)
{
    return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

static int is_url(const char *s, size_t n)
{
    return starts(s, n, 0, "https://") || starts(s, n, 0, "http://");
}

/* Bytes a backslash escapes, like Discord: any ASCII punctuation, or a whole non-ASCII character. */
static size_t escaped_len(const char *s, size_t n, size_t i)
{
    unsigned char c;

    if (i >= n)
        return 0;
    c = (unsigned char)s[i];
    if (c < 0x80)
        return c > ' ' && !is_alnum((char)c) ? 1 : 0;
    if (c == 0xEE && i + 1 < n && (unsigned char)s[i + 1] == 0x80)
        return 0; /* our own mention and emoji markers */
    {
        size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return i + len <= n ? len : 0;
    }
}

static size_t run_of(const char *s, size_t n, size_t i, char c)
{
    size_t k = i;

    while (k < n && s[k] == c)
        k++;
    return k - i;
}

size_t md_code_end(const char *s, size_t n, size_t i)
{
    size_t r = run_of(s, n, i, '`');

    if (!r)
        return 0;
    for (size_t j = i + r; j < n;) {
        size_t k = run_of(s, n, j, '`');
        if (k == r && j > i + r)
            return j + k;
        j += k ? k : 1;
    }
    return 0;
}

/* The ")" closing a link's "(" at s[i], nested parentheses included; NPOS if none. */
static size_t close_paren(const char *s, size_t n, size_t i)
{
    int depth = 0;

    for (size_t j = i; j < n && s[j] != '\n'; j++) {
        if (s[j] == '(')
            depth++;
        else if (s[j] == ')' && --depth == 0)
            return j;
    }
    return NPOS;
}

/* End of a bare URL: stops at whitespace, drops trailing punctuation. */
static size_t url_end(const char *s, size_t n, size_t i)
{
    size_t j = i;
    int parens = 0;

    while (j < n && !is_space(s[j]) && s[j] != '<' && s[j] != '>') {
        if (s[j] == '(')
            parens++;
        else if (s[j] == ')')
            parens--;
        j++;
    }
    while (j > i) {
        char c = s[j - 1];
        if (c == '.' || c == ',' || c == ':' || c == ';' || c == '!' || c == '?' || c == '"' || c == '\'' ||
            (c == ')' && parens < 0)) {
            if (c == ')')
                parens++;
            j--;
        } else {
            break;
        }
    }
    return j;
}

/* ---- Inline styles ---- */

static void inline_md(md_doc_t *d, const char *s, size_t n, unsigned flags, int link)
{
    static const struct {
        const char *tok;
        unsigned flag;
    } pairs[] = {{"||", MD_SPOILER}, {"**", MD_BOLD}, {"__", MD_UNDERLINE}, {"~~", MD_STRIKE}};
    size_t i = 0, run = 0;

#define FLUSH()                                          \
    do {                                                 \
        if (i > run)                                     \
            emit(d, s + run, i - run, flags, link);      \
    } while (0)

    while (i < n) {
        char c = s[i];
        size_t j, k;
        int matched = 0;

        if (c == '\\' && (k = escaped_len(s, n, i + 1)) != 0) {
            FLUSH();
            emit(d, s + i + 1, k, flags, link);
            i = run = i + 1 + k;
            continue;
        }
        if (starts(s, n, i, MD_EDITED_MARK)) {
            FLUSH();
            emit(d, "(edited)", 8, MD_EDITED, -1);
            i = run = i + 3;
            continue;
        }
        if (starts(s, n, i, MD_EMOJI_OPEN) && (j = find(s, n, i + 3, MD_EMOJI_CLOSE)) != NPOS) {
            size_t colon = find(s, j, i + 3, ":");
            FLUSH();
            if (colon != NPOS) {
                int idx = add_link(d, s + i + 3, colon - i - 3);
                emit(d, "\xEF\xBF\xBC", 3, flags | MD_EMOJI, idx); /* U+FFFC */
            }
            i = run = j + 3;
            continue;
        }
        if (starts(s, n, i, MD_MENTION_OPEN) && (j = find(s, n, i + 3, MD_MENTION_CLOSE)) != NPOS) {
            FLUSH();
            inline_md(d, s + i + 3, j - i - 3, flags | MD_MENTION, link);
            i = run = j + 3;
            continue;
        }
        /* `code`, ``co`de``, and ```code``` within a line */
        if (c == '`' && (j = md_code_end(s, n, i)) != 0) {
            k = run_of(s, n, i, '`');
            FLUSH();
            emit(d, s + i + k, j - i - 2 * k, flags | MD_CODE, link);
            i = run = j;
            continue;
        }
        /* ||spoiler|| **bold** __underline__ ~~strike~~ */
        for (int p = 0; p < (int)(sizeof pairs / sizeof pairs[0]) && !matched; p++) {
            if (!starts(s, n, i, pairs[p].tok))
                continue;
            j = find(s, n, i + 2, pairs[p].tok);
            if (j == NPOS || j == i + 2)
                break;
            /* "***" closes bold after the italic: take the last pair of the run. */
            while (j + 2 < n && s[j + 2] == pairs[p].tok[1])
                j++;
            FLUSH();
            inline_md(d, s + i + 2, j - i - 2, flags | pairs[p].flag, link);
            i = run = j + 2;
            matched = 1;
        }
        if (matched)
            continue;
        /*
         * *italic* or _italic_, ending on a matching char not preceded by a space,
         * across lines too. A "**" inside is bold, not the end; a lone "**" is text.
         */
        if ((c == '*' || c == '_') && i + 1 < n && !is_space(s[i + 1]) && s[i + 1] != c &&
            (c == '*' || i == 0 || !is_alnum(s[i - 1]))) {
            for (j = i + 1; j < n; j++) {
                if (s[j] == c && j + 1 < n && s[j + 1] == c && c == '*') {
                    j++;
                    continue;
                }
                if (s[j] == c && !is_space(s[j - 1]) && (c == '*' || j + 1 >= n || !is_alnum(s[j + 1])))
                    break;
            }
            if (j < n && s[j] == c && j > i + 1) {
                FLUSH();
                inline_md(d, s + i + 1, j - i - 1, flags | MD_ITALIC, link);
                i = run = j + 1;
                continue;
            }
        }
        /* [text](https://...) or [text](<https://...>): the text ends at the first "]" */
        if (c == '[' && !(flags & MD_LINK) && (j = find(s, n, i + 1, "]")) != NPOS && j + 1 < n && s[j + 1] == '(' &&
            find(s, j, i, "\n") == NPOS && (k = close_paren(s, n, j + 1)) != NPOS) {
            size_t a = j + 2, b = k;
            if (b > a + 1 && s[a] == '<' && s[b - 1] == '>') {
                a++;
                b--;
            }
            if (is_url(s + a, b - a)) {
                int idx = add_link(d, s + a, b - a);
                FLUSH();
                inline_md(d, s + i + 1, j - i - 1, flags | MD_LINK, idx);
                i = run = k + 1;
                continue;
            }
        }
        /* Bare link, or <link> which Discord shows without a preview. */
        if (!(flags & MD_LINK) && (i == 0 || !is_alnum(s[i - 1])) &&
            (starts(s, n, i, "https://") || starts(s, n, i, "http://") ||
             (c == '<' && (starts(s, n, i + 1, "https://") || starts(s, n, i + 1, "http://"))))) {
            size_t from = c == '<' ? i + 1 : i;
            int idx;
            j = url_end(s, n, from);
            idx = add_link(d, s + from, j - from);
            FLUSH();
            emit(d, s + from, j - from, flags | MD_LINK, idx);
            if (c == '<' && j < n && s[j] == '>')
                j++;
            i = run = j;
            continue;
        }
        i++;
    }
    FLUSH();
#undef FLUSH
}

/* ---- Blocks ---- */

static void add_block(md_doc_t *d, int kind, const char *s, size_t n, int raw, int quoted)
{
    int start = d->len;

    if (raw)
        emit(d, s, n, 0, -1);
    else
        inline_md(d, s, n, 0, -1);
    if (d->len == start)
        return;
    if (d->nblocks == d->cap_blocks) {
        d->cap_blocks = d->cap_blocks ? d->cap_blocks * 2 : 8;
        d->blocks = mem_realloc(d->blocks, (size_t)d->cap_blocks * sizeof *d->blocks);
    }
    d->blocks[d->nblocks].kind = quoted && kind == MD_PARA ? MD_QUOTE : kind;
    d->blocks[d->nblocks].quoted = quoted;
    d->blocks[d->nblocks].start = start;
    d->blocks[d->nblocks].len = d->len - start;
    d->nblocks++;
}

static void flush(md_doc_t *d, int kind, sb_t *pending, int quoted)
{
    if (pending->len)
        add_block(d, kind, pending->data, pending->len, 0, quoted);
    sb_clear(pending);
}

/* A list item: "- ", "* " or "1. ", after up to 8 spaces of nesting. Sets the marker's end and the level. */
static int list_item(const char *s, size_t n, size_t i, size_t end, size_t *from, int *level, int *number)
{
    size_t k = i, digits;

    while (k < end && s[k] == ' ' && k - i < 8)
        k++;
    *level = (int)(k - i) / 2;
    *number = -1; /* a bullet */
    if (starts(s, n, k, "- ") || starts(s, n, k, "* ")) {
        *from = k + 2;
        return 1;
    }
    for (digits = 0; k + digits < end && s[k + digits] >= '0' && s[k + digits] <= '9' && digits < 9; digits++)
        *number = (*number < 0 ? 0 : *number * 10) + (s[k + digits] - '0');
    if (digits && starts(s, n, k + digits, ". ")) {
        *from = k + digits + 2;
        return 1;
    }
    *number = -1;
    return 0;
}

/* Emoji-only messages (custom or unicode, up to 30) are drawn large, like Discord does. */
static int only_emoji(const md_doc_t *d)
{
    int count = 0;

    for (int i = 0; i < d->len; i++) {
        wchar_t c = d->text[i];
        if (c == ' ' || c == '\n' || c == 0xFE0F || c == 0x200D || (c >= 0xDC00 && c <= 0xDFFF))
            continue;
        if (d->nspans && d->spans[d->nspans - 1].flags == MD_EDITED && i >= d->spans[d->nspans - 1].start)
            break; /* the "(edited)" label does not count */
        if (c == 0xFFFC || (c >= 0x2190 && c <= 0x2BFF) || c == 0x00A9 || c == 0x00AE ||
            (c >= 0xD83C && c <= 0xD83E)) { /* U+1F000..U+1FAFF */
            count++;
            continue;
        }
        return 0;
    }
    return count > 0 && count <= 30;
}

/* The blocks of s[0..n); inside a quote (quoted), "> " is plain text: Discord quotes one level. */
static void parse_blocks(md_doc_t *doc, const char *s, size_t n, int quoted)
{
    sb_t pending = {0};
    int pending_kind = MD_PARA;
    size_t i = 0;

    while (i < n) {
        size_t end = i, from;
        int kind = MD_PARA, level, number;

        while (end < n && s[end] != '\n')
            end++;

        /* ```lang\ncode``` */
        if (starts(s, n, i, "```")) {
            size_t j = find(s, n, i + 3, "```");
            if (j != NPOS) {
                size_t a = i + 3, b = j, nl = find(s, j, a, "\n");
                if (nl != NPOS) {
                    int tag = 1;
                    for (size_t k = a; k < nl; k++)
                        if (!is_alnum(s[k]) && s[k] != '+' && s[k] != '-' && s[k] != '#')
                            tag = 0;
                    if (tag)
                        a = nl + 1;
                }
                while (b > a && (s[b - 1] == '\n' || s[b - 1] == '\r'))
                    b--;
                flush(doc, pending_kind, &pending, quoted);
                add_block(doc, MD_CODEBLOCK, s + a, b - a, 1, quoted);
                i = j + 3;
                if (i < n && s[i] == '\n')
                    i++;
                continue;
            }
        }
        if (!quoted) {
            /* >>> quotes the rest of the message, "> " each line: their content has blocks of its own. */
            if (starts(s, n, i, ">>> ")) {
                flush(doc, pending_kind, &pending, quoted);
                parse_blocks(doc, s + i + 4, n - i - 4, 1);
                break;
            }
            if (starts(s, n, i, "> ")) {
                sb_t inner = {0};
                flush(doc, pending_kind, &pending, quoted);
                while (i < n && starts(s, n, i, "> ")) {
                    for (end = i; end < n && s[end] != '\n'; end++)
                        ;
                    if (inner.len)
                        sb_add(&inner, "\n");
                    sb_addn(&inner, s + i + 2, end - i - 2);
                    i = end + (end < n);
                }
                parse_blocks(doc, inner.data ? inner.data : "", inner.len, 1);
                sb_free(&inner);
                continue;
            }
        }

        from = i;
        if (starts(s, n, i, "### ")) {
            kind = MD_H3;
            from = i + 4;
        } else if (starts(s, n, i, "## ")) {
            kind = MD_H2;
            from = i + 3;
        } else if (starts(s, n, i, "# ")) {
            kind = MD_H1;
            from = i + 2;
        } else if (starts(s, n, i, "-# ")) {
            kind = MD_SUBTEXT;
            from = i + 3;
        } else if (list_item(s, n, i, end, &from, &level, &number)) {
            kind = MD_LIST;
        }

        if (kind == MD_H1 || kind == MD_H2 || kind == MD_H3 || kind == MD_SUBTEXT) {
            flush(doc, pending_kind, &pending, quoted);
            add_block(doc, kind, s + from, end - from, 0, quoted);
        } else {
            if (kind != pending_kind)
                flush(doc, pending_kind, &pending, quoted);
            pending_kind = kind;
            if (pending.len)
                sb_add(&pending, "\n");
            if (kind == MD_LIST) {
                for (int l = 0; l < level && l < 4; l++)
                    sb_add(&pending, "\xE2\x80\x83\xE2\x80\x83"); /* two em spaces per level */
                if (number >= 0) {
                    sb_i64(&pending, number);
                    sb_add(&pending, ".  ");
                } else {
                    sb_add(&pending, level ? "\xE2\x97\xA6  " : "\xE2\x80\xA2  "); /* white bullet nested */
                }
            }
            sb_addn(&pending, s + from, end - from);
        }
        i = end + 1;
    }
    flush(doc, pending_kind, &pending, quoted);
    sb_free(&pending);
}

void md_parse(const char *s, size_t n, md_doc_t *doc)
{
    parse_blocks(doc, s, n, 0);
    doc->jumbo = doc->nblocks == 1 && doc->blocks[0].kind == MD_PARA && only_emoji(doc);
}

void md_free(md_doc_t *doc)
{
    mem_free(doc->text);
    mem_free(doc->blocks);
    mem_free(doc->spans);
    sb_free(&doc->links);
    *doc = (md_doc_t){0};
}
