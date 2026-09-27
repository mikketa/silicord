#pragma once
#include <stddef.h>
#include "sb.h"

/* RFC 4648 base64. The url variant uses '-' and '_' and omits padding. */
void b64_encode(const unsigned char *data, size_t n, sb_t *out);
void b64url_encode(const unsigned char *data, size_t n, sb_t *out);
/* Accepts both alphabets, with or without padding. Returns 0 on invalid input. */
int b64_decode(const char *s, size_t n, sb_t *out);
