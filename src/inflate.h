#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * Inflater for the gateway's zlib-stream: one zlib stream for the whole
 * connection, each message ending with a sync flush (00 00 FF FF). Matches may
 * point back into earlier messages, so the last 32 KB of output are kept.
 */

/* A Huffman code: canonical counts and symbols, plus a lookup table for codes up to 9 bits. */
typedef struct {
    short count[16];
    short symbol[288];
    unsigned short fast[512]; /* by the next 9 input bits: length << 9 | symbol, 0 when longer */
} inflate_code_t;

typedef struct {
    int started;              /* the 2-byte zlib header was read */
    int done;                 /* the final block was seen */
    int fixed_ready;
    unsigned long long total; /* bytes produced so far */
    inflate_code_t lencode, distcode, fixed_len, fixed_dist;
    unsigned char window[32768];
} inflate_t;

void inflate_init(inflate_t *z);
/* Inflates one message's compressed bytes, appending the output. Returns 0 on corrupt data. */
int inflate_message(inflate_t *z, const unsigned char *in, size_t n, sb_t *out);
/* The bytes end with the sync flush marker: a message is complete. */
int inflate_complete(const unsigned char *in, size_t n);
