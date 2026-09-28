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
/* Discord's other names (emoji_alias.c), sorted: "name\0emoji\0" each; not in the picker. */
extern const char k_emoji_aliases[];
extern const int k_nemoji_aliases;

/* The emoji called `name` (without colons), by its picker name or another Discord name; NULL if none. */
const char *emoji_by_name(const char *name, size_t n);
/*
 * Appends the emoji called `name` to out, skin tone variants included
 * ("thumbsup_tone2", "wave_medium_skin_tone"). Returns 0 if there is none.
 */
int emoji_append(const char *name, size_t n, sb_t *out);
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
