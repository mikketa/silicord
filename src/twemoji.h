#pragma once
#include <stddef.h>

/* Unicode emoji as Discord draws them: Twemoji, an SVG each on Discord's servers. */

typedef struct {
    const wchar_t *seq;       /* the emoji in UTF-16, without U+FE0F */
    unsigned long long asset; /* its SVG: https://discord.com/assets/<16 hex digits>.svg */
} twemoji_t;

extern const twemoji_t k_twemoji[];
extern const int k_ntwemoji;

enum {
    TWEMOJI_ANY,   /* every character Discord has an image for, as in messages (a bare "™" too) */
    TWEMOJI_COLOR, /* only what a font draws in color: emoji by default, or followed by U+FE0F */
};

/*
 * The longest emoji starting s[0, n) (skin tones, ZWJ sequences, flags, keycaps
 * and tags included): returns how many UTF-16 units it takes, a U+FE0F after
 * it too, and its index in k_twemoji in *index; 0 when none starts there.
 */
int twemoji_match(const wchar_t *s, int n, int mode, int *index);
/* Emoji `index`'s SVG, "https://discord.com/assets/<asset>.svg", into out (at least 64 bytes). */
void twemoji_url(int index, char *out);
