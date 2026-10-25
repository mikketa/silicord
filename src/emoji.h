#pragma once
#include <stddef.h>
#include "sb.h"

/* Unicode emoji and their short names (":thumbsup:"), in Discord's picker order. */

typedef struct {
    const char *name;
    int first, end;      /* range of emoji indices */
} emoji_category_t;

#define EMOJI_CATEGORIES 8

/*
 * The emoji one after another, each "emoji\0names\0": the emoji in UTF-8, its
 * short names space-separated, the main one first. No pointers: the table
 * needs no relocations, and stays in the exe's pages.
 */
extern const char k_emoji_data[];
extern const int k_nemoji;
extern const emoji_category_t k_emoji_categories[EMOJI_CATEGORIES];

/* Emoji i (UTF-8), and its names. The first call indexes the table: from one thread at a time, the UI's. */
const char *emoji_text(int i);
const char *emoji_names(int i);
/* Discord's other names (emoji_alias.c), sorted: "name\0emoji\0" each; not in the picker. */
extern const char k_emoji_aliases[];
extern const int k_nemoji_aliases;

/* The emoji called `name` (without colons), by its picker name or another Discord name; NULL if none. */
const char *emoji_by_name(const char *name, size_t n);
/* 2 when one of emoji i's names starts with `query`, 1 when one contains it, else 0 (ASCII, any case). */
int emoji_matches(int i, const char *query);
/* Copies emoji i's main name into out. */
void emoji_main_name(int i, char *out, size_t size);

/*
 * Rewrites ":name:" in a message about to be sent, as Discord does: custom
 * emoji through `custom` (returns "<:name:id>" or NULL), then unicode ones.
 * Code spans and blocks are left alone.
 */
typedef const char *(*emoji_custom_fn)(void *ctx, const char *name, size_t n);
void emoji_expand(const char *s, size_t n, sb_t *out, emoji_custom_fn custom, void *ctx);
