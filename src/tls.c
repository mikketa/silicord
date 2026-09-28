#include "tls.h"

void tls_u8(sb_t *out, unsigned v)
{
    char b = (char)(unsigned char)v;

    sb_addn(out, &b, 1);
}

void tls_u16(sb_t *out, unsigned v)
{
    tls_u8(out, v >> 8);
    tls_u8(out, v);
}

void tls_u32(sb_t *out, unsigned long v)
{
    tls_u16(out, (unsigned)(v >> 16));
    tls_u16(out, (unsigned)v);
}

void tls_u64(sb_t *out, unsigned long long v)
{
    tls_u32(out, (unsigned long)(v >> 32));
    tls_u32(out, (unsigned long)v);
}

void tls_varint(sb_t *out, size_t v)
{
    if (v < 0x40)
        tls_u8(out, (unsigned)v);
    else if (v < 0x4000)
        tls_u16(out, (unsigned)(0x4000 | v));
    else
        tls_u32(out, (unsigned long)(0x80000000ul | v));
}

void tls_vec(sb_t *out, const void *data, size_t n)
{
    tls_varint(out, n);
    if (n)
        sb_addn(out, data, n);
}

void tls_reader(tls_reader_t *r, const void *data, size_t n)
{
    r->p = data;
    r->end = r->p + n;
    r->bad = 0;
}

const unsigned char *tls_read_raw(tls_reader_t *r, size_t n)
{
    const unsigned char *p = r->p;

    if (r->bad || (size_t)(r->end - r->p) < n) {
        r->bad = 1;
        return NULL;
    }
    r->p += n;
    return p;
}

unsigned tls_read_u8(tls_reader_t *r)
{
    const unsigned char *p = tls_read_raw(r, 1);

    return p ? p[0] : 0;
}

unsigned tls_read_u16(tls_reader_t *r)
{
    const unsigned char *p = tls_read_raw(r, 2);

    return p ? (unsigned)p[0] << 8 | p[1] : 0;
}

unsigned long tls_read_u32(tls_reader_t *r)
{
    unsigned long hi = tls_read_u16(r);

    return hi << 16 | tls_read_u16(r);
}

unsigned long long tls_read_u64(tls_reader_t *r)
{
    unsigned long long hi = tls_read_u32(r);

    return hi << 32 | tls_read_u32(r);
}

size_t tls_read_varint(tls_reader_t *r)
{
    unsigned first = tls_read_u8(r);
    size_t v;

    switch (first >> 6) {
    case 0:
        return first;
    case 1:
        v = (size_t)(first & 0x3f) << 8 | tls_read_u8(r);
        if (v < 0x40)
            r->bad = 1; /* RFC 9420: the minimum size is required */
        return v;
    case 2:
        v = (size_t)(first & 0x3f) << 24;
        v |= (size_t)tls_read_u8(r) << 16;
        v |= (size_t)tls_read_u16(r);
        if (v < 0x4000)
            r->bad = 1;
        return v;
    default:
        r->bad = 1;
        return 0;
    }
}

const unsigned char *tls_read_vec(tls_reader_t *r, size_t *n)
{
    *n = tls_read_varint(r);
    if (r->bad) {
        *n = 0;
        return NULL;
    }
    return tls_read_raw(r, *n);
}

tls_reader_t tls_read_nested(tls_reader_t *r)
{
    tls_reader_t sub;
    size_t n;
    const unsigned char *p = tls_read_vec(r, &n);

    tls_reader(&sub, p, n);
    sub.bad = r->bad;
    return sub;
}

int tls_done(const tls_reader_t *r)
{
    return !r->bad && r->p == r->end;
}
