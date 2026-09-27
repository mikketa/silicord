#pragma once
#include <stddef.h>

/* Growable UTF-8 string. `data` is always NUL-terminated once anything was added. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sb_t;

void sb_reserve(sb_t *sb, size_t extra);
void sb_addn(sb_t *sb, const char *s, size_t n);
void sb_add(sb_t *sb, const char *s);
void sb_u64(sb_t *sb, unsigned long long v);
void sb_i64(sb_t *sb, long long v);
/* Appends `s` as a quoted, escaped JSON string. */
void sb_json_str(sb_t *sb, const char *s, size_t n);
void sb_clear(sb_t *sb);
/* Wipes the contents before freeing: use for anything holding the token. */
void sb_free(sb_t *sb);
