#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * Allocation-free JSON reader. A json_t is a slice [p, end) of the source
 * buffer holding exactly one value; nothing is copied until json_str().
 */
typedef struct {
    const char *p;
    const char *end;
} json_t;

typedef struct {
    const char *p;
    const char *end;
    int is_object;
} json_iter_t;

enum {
    JSON_INVALID,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
};

/* Parses a whole document. Returns 0 if it is not a single valid value. */
int json_parse(const char *s, size_t n, json_t *out);
int json_type(json_t v);

/* Looks up `key` in an object. Keys are compared raw (no escape decoding). */
int json_get(json_t obj, const char *key, json_t *out);

/* Iterates over an array or object; `key` may be NULL. */
void json_iter(json_t v, json_iter_t *it);
int json_next(json_iter_t *it, json_t *key, json_t *val);
size_t json_count(json_t v);

int json_int(json_t v, long long *out);
/* Decodes a string value (escapes, \u surrogate pairs) and appends it as UTF-8. */
int json_str(json_t v, sb_t *out);
/* Copies a short string (without decoding escapes) or number into dst, NUL-terminated. */
void json_raw(json_t v, char *dst, size_t size);
/* Compares a string value with `s` without decoding escapes. */
int json_str_eq(json_t v, const char *s);
