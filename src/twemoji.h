#pragma once
#include <stddef.h>

/*
 * Unicode emoji as Discord draws them: Twemoji, an SVG each on Discord's
 * servers. Emoji k is k_twemoji_text from k_twemoji_end[k - 1] (0 for the
 * first) to k_twemoji_end[k], in UTF-16 without U+FE0F, sorted by code units;
 * its SVG is https://discord.com/assets/<k_twemoji_asset[k], 16 hex digits>.svg.
 * No pointers: the table needs no relocations, and stays in the exe's pages.
 */
extern const wchar_t k_twemoji_text[];
extern const unsigned short k_twemoji_end[];
extern const unsigned long long k_twemoji_asset[];
extern const int k_ntwemoji;

/* Emoji k's text: returns its length in units, its first in *s. */
int twemoji_text(int k, const wchar_t **s);

enum {
    TWEMOJI_ANY,   /* every character Discord has an image for, as in messages (a bare "™" too) */
    TWEMOJI_COLOR, /* only what a font draws in color: emoji by default, or followed by U+FE0F */
};

/*
 * The longest emoji starting s[0, n) (skin tones, ZWJ sequences, flags, keycaps
 * and tags included): returns how many UTF-16 units it takes, a U+FE0F after
 * it too, and its index in the table in *index; 0 when none starts there.
 */
int twemoji_match(const wchar_t *s, int n, int mode, int *index);
/* Emoji `index`'s SVG, "https://discord.com/assets/<asset>.svg", into out (at least 64 bytes). */
void twemoji_url(int index, char *out);
