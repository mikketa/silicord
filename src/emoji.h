#pragma once
#include <stddef.h>
#include "sb.h"

/* Unicode emoji and their short names (":thumbsup:"), in Discord's picker order. */

typedef struct {
    const char *emoji;   /* UTF-8 */
    const char *names;   /* space-separated short names, the first one is the main one */
} emoji_t;

typedef struct {
    const char *name;
    int first, end;      /* range in k_emoji */
} emoji_category_t;

#define EMOJI_CATEGORIES 8

extern const emoji_t k_emoji[];
extern const int k_nemoji;
extern const emoji_category_t k_emoji_categories[EMOJI_CATEGORIES];

/* The emoji called `name` (without colons), NULL if there is none. */
const char *emoji_by_name(const char *name, size_t n);
/* Whether one of emoji i's names contains `query` (case-insensitive ASCII). */
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
