#pragma once
#include <stddef.h>
#include "sb.h"

/* UTF-8 -> UTF-16, NUL-terminated, allocated with mem_alloc. */
wchar_t *utf8_to_wide(const char *s, size_t n);
/* UTF-16 -> UTF-8, appended to `out`. */
void wide_to_utf8(const wchar_t *w, size_t n, sb_t *out);
