#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * Discord-flavoured markdown, parsed into UTF-16 text split in blocks, with
 * styled spans. Mentions arrive wrapped in MD_MENTION_OPEN / MD_MENTION_CLOSE
 * (private-use characters inserted by the message formatter).
 */

#define MD_MENTION_OPEN "\xEE\x80\x80"  /* U+E000 */
#define MD_MENTION_CLOSE "\xEE\x80\x81" /* U+E001 */

enum {
    MD_BOLD = 1,
    MD_ITALIC = 2,
    MD_UNDERLINE = 4,
    MD_STRIKE = 8,
    MD_CODE = 16,
    MD_SPOILER = 32,
    MD_LINK = 64,
    MD_MENTION = 128,
};

enum { MD_PARA, MD_QUOTE, MD_CODEBLOCK, MD_H1, MD_H2, MD_H3, MD_SUBTEXT, MD_LIST };

typedef struct {
    int kind;
    int start, len;   /* UTF-16 range in md_doc_t.text */
} md_block_t;

typedef struct {
    int start, len;   /* UTF-16 range in md_doc_t.text */
    unsigned flags;
    int link;         /* index into the links, -1 if none */
} md_span_t;

typedef struct {
    wchar_t *text;
    int len, cap;
    md_block_t *blocks;
    int nblocks, cap_blocks;
    md_span_t *spans;
    int nspans, cap_spans;
    sb_t links;       /* URLs, NUL-separated */
    int nlinks;
} md_doc_t;

void md_parse(const char *s, size_t n, md_doc_t *doc);
void md_free(md_doc_t *doc);
const char *md_link(const md_doc_t *doc, int i);
