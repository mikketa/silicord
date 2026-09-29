#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * The TLS presentation language as MLS (RFC 9420) uses it: big-endian
 * integers and vectors whose length is a variable-size integer (1, 2 or 4
 * bytes, the top two bits giving the size).
 */

void tls_u8(sb_t *out, unsigned v);
void tls_u16(sb_t *out, unsigned v);
void tls_u32(sb_t *out, unsigned long v);
void tls_u64(sb_t *out, unsigned long long v);
void tls_varint(sb_t *out, size_t v);
/* opaque data<V>: length then bytes. */
void tls_vec(sb_t *out, const void *data, size_t n);

/* A reader over bytes; any read past the end or malformed length sets `bad`. */
typedef struct {
    const unsigned char *p, *end;
    int bad;
} tls_reader_t;

void tls_reader(tls_reader_t *r, const void *data, size_t n);
unsigned tls_read_u8(tls_reader_t *r);
unsigned tls_read_u16(tls_reader_t *r);
unsigned long tls_read_u32(tls_reader_t *r);
unsigned long long tls_read_u64(tls_reader_t *r);
size_t tls_read_varint(tls_reader_t *r);
/* A vector's bytes, left in place: *data points into the input. */
const unsigned char *tls_read_vec(tls_reader_t *r, size_t *n);
/* A sub-reader over the next vector, for nested structures. */
tls_reader_t tls_read_nested(tls_reader_t *r);
int tls_done(const tls_reader_t *r);
