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

static int escapable(char c)
{
    const char *set = "*_~`|\\<>#-[]():@";

    for (; *set; set++)
        if (*set == c)
            return 1;
    return 0;
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

        if (c == '\\' && i + 1 < n && escapable(s[i + 1])) {
            FLUSH();
            emit(d, s + i + 1, 1, flags, link);
            i = run = i + 2;
            continue;
        }
        if (starts(s, n, i, MD_MENTION_OPEN) && (j = find(s, n, i + 3, MD_MENTION_CLOSE)) != NPOS) {
            FLUSH();
            inline_md(d, s + i + 3, j - i - 3, flags | MD_MENTION, link);
            i = run = j + 3;
            continue;
        }
        if (c == '`') {
            const char *tok = starts(s, n, i, "``") ? "``" : "`";
            k = (size_t)lstrlenA(tok);
            j = find(s, n, i + k, tok);
            if (j != NPOS && j > i + k) {
                FLUSH();
                emit(d, s + i + k, j - i - k, flags | MD_CODE, link);
                i = run = j + k;
                continue;
            }
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
        /* *italic* or _italic_, ending on a matching char not preceded by a space. */
        if ((c == '*' || c == '_') && i + 1 < n && !is_space(s[i + 1]) &&
            (c == '*' || i == 0 || !is_alnum(s[i - 1]))) {
            for (j = i + 1; j < n && s[j] != '\n'; j++)
                if (s[j] == c && !is_space(s[j - 1]) && (c == '*' || j + 1 >= n || !is_alnum(s[j + 1])))
                    break;
            if (j < n && s[j] == c) {
                FLUSH();
                inline_md(d, s + i + 1, j - i - 1, flags | MD_ITALIC, link);
                i = run = j + 1;
                continue;
            }
        }
        /* [text](https://...) */
        if (c == '[' && !(flags & MD_LINK) && (j = find(s, n, i + 1, "](")) != NPOS &&
            (k = find(s, n, j + 2, ")")) != NPOS && is_url(s + j + 2, k - j - 2) && find(s, j, i, "\n") == NPOS) {
            int idx = add_link(d, s + j + 2, k - j - 2);
            FLUSH();
            inline_md(d, s + i + 1, j - i - 1, flags | MD_LINK, idx);
            i = run = k + 1;
            continue;
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

static void add_block(md_doc_t *d, int kind, const char *s, size_t n, int raw)
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
    d->blocks[d->nblocks].kind = kind;
    d->blocks[d->nblocks].start = start;
    d->blocks[d->nblocks].len = d->len - start;
    d->nblocks++;
}

static void flush(md_doc_t *d, int kind, sb_t *pending)
{
    if (pending->len)
        add_block(d, kind, pending->data, pending->len, 0);
    sb_clear(pending);
}

void md_parse(const char *s, size_t n, md_doc_t *doc)
{
    sb_t pending = {0};
    int pending_kind = MD_PARA;
    size_t i = 0;

    while (i < n) {
        size_t end = i, from;
        int kind = MD_PARA;

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
                flush(doc, pending_kind, &pending);
                add_block(doc, MD_CODEBLOCK, s + a, b - a, 1);
                i = j + 3;
                if (i < n && s[i] == '\n')
                    i++;
                continue;
            }
        }
        /* >>> quotes the rest of the message */
        if (starts(s, n, i, ">>> ")) {
            flush(doc, pending_kind, &pending);
            add_block(doc, MD_QUOTE, s + i + 4, n - i - 4, 0);
            break;
        }

        from = i;
        if (starts(s, n, i, "> ")) {
            kind = MD_QUOTE;
            from = i + 2;
        } else if (end == i + 1 && s[i] == '>') {
            kind = MD_QUOTE;
            from = end;
        } else if (starts(s, n, i, "### ")) {
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
        } else if (starts(s, n, i, "- ") || starts(s, n, i, "* ")) {
            kind = MD_LIST;
            from = i + 2;
        }

        if (kind == MD_H1 || kind == MD_H2 || kind == MD_H3 || kind == MD_SUBTEXT) {
            flush(doc, pending_kind, &pending);
            add_block(doc, kind, s + from, end - from, 0);
        } else {
            if (kind != pending_kind)
                flush(doc, pending_kind, &pending);
            pending_kind = kind;
            if (pending.len)
                sb_add(&pending, "\n");
            if (kind == MD_LIST)
                sb_add(&pending, "\xE2\x80\xA2  "); /* bullet */
            sb_addn(&pending, s + from, end - from);
        }
        i = end + 1;
    }
    flush(doc, pending_kind, &pending);
    sb_free(&pending);
}

void md_free(md_doc_t *doc)
{
    mem_free(doc->text);
    mem_free(doc->blocks);
    mem_free(doc->spans);
    sb_free(&doc->links);
    *doc = (md_doc_t){0};
}
