#pragma once
#include "sb.h"

/*
 * Discord's search filters typed in the search box:
 * "hello from:ann has:image in:general before:2026-09-01 pinned:true".
 */
enum { SF_FROM, SF_MENTIONS, SF_HAS, SF_IN, SF_BEFORE, SF_AFTER, SF_DURING, SF_PINNED };

typedef struct {
    int key;          /* SF_* */
    char value[64];   /* as typed, without a leading @ or # */
} search_filter_t;

/* Splits `q` into up to `max` filters and the words left, appended to `content`. Returns the filter count. */
int search_parse(const char *q, search_filter_t *f, int max, sb_t *content);
/* The first snowflake of day "YYYY-MM-DD" (UTC), or of the day after with `next_day`; 0 if not a date. */
unsigned long long search_day_snowflake(const char *date, int next_day);
