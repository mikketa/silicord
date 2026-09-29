#include <string.h>
#include "inflate.h"

/* DEFLATE (RFC 1951) decoding, after Mark Adler's puff, with a table for short codes. */

#define FAST_BITS 9
#define MAX_BITS 15
#define WINDOW 32768u

typedef struct {
    const unsigned char *p, *end;
    unsigned long long bits;
    int count;
    int error;
} bits_t;

static void refill(bits_t *b)
{
    while (b->count <= 56 && b->p < b->end) {
        b->bits |= (unsigned long long)*b->p++ << b->count;
        b->count += 8;
    }
}

/* The next n bits (n <= 32), least significant first. */
static unsigned take(bits_t *b, int n)
{
    unsigned v;

    if (b->count < n) {
        refill(b);
        if (b->count < n) {
            b->error = 1;
            return 0;
        }
    }
    v = (unsigned)(b->bits & ((1ull << n) - 1));
    b->bits >>= n;
    b->count -= n;
    return v;
}

/* Builds a code from the bit lengths of its n symbols. Returns 0 if over-subscribed. */
static int build(inflate_code_t *h, const unsigned char *length, int n)
{
    short offs[MAX_BITS + 1];
    unsigned next[MAX_BITS + 1];
    int left = 1;

    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int s = 0; s < n; s++)
        h->count[length[s]]++;
    if (h->count[0] == n)
        return 1; /* no codes: fine for an unused distance code */
    for (int len = 1; len <= MAX_BITS; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0)
            return 0;
    }
    offs[1] = 0;
    for (int len = 1; len < MAX_BITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int s = 0; s < n; s++)
        if (length[s])
            h->symbol[offs[length[s]]++] = (short)s;
    /* Canonical codes, bit-reversed as they arrive, fill the lookup table. */
    next[1] = 0;
    for (int len = 2; len <= MAX_BITS; len++)
        next[len] = (next[len - 1] + (unsigned)h->count[len - 1]) << 1;
    for (int s = 0; s < n; s++) {
        int len = length[s];
        unsigned code, rev = 0;
        if (!len)
            continue;
        code = next[len]++;
        if (len > FAST_BITS)
            continue;
        for (int k = 0; k < len; k++)
            rev |= ((code >> k) & 1u) << (len - 1 - k);
        for (unsigned k = rev; k < (1u << FAST_BITS); k += 1u << len)
            h->fast[k] = (unsigned short)(len << 9 | s);
    }
    return 1;
}

static int decode(bits_t *b, const inflate_code_t *h)
{
    unsigned e;
    int code = 0, first = 0, index = 0;

    if (b->count < MAX_BITS)
        refill(b);
    e = h->fast[b->bits & ((1u << FAST_BITS) - 1)];
    if (e && (int)(e >> 9) <= b->count) {
        b->bits >>= e >> 9;
        b->count -= (int)(e >> 9);
        return (int)(e & 511);
    }
    for (int len = 1; len <= MAX_BITS; len++) {
        int count;
        if (!b->count) {
            b->error = 1;
            return -1;
        }
        code |= (int)(b->bits & 1);
        b->bits >>= 1;
        b->count--;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    b->error = 1;
    return -1;
}

static const short k_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short k_dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Literals and matches until the end of the block. `start` is where this message's output begins. */
static int codes(inflate_t *z, bits_t *b, const inflate_code_t *lc, const inflate_code_t *dc, sb_t *out, size_t start)
{
    for (;;) {
        int sym = decode(b, lc), len, dist;
        size_t made, back;
        if (sym < 0)
            return 0;
        if (out->len + 259 > out->cap)
            sb_reserve(out, 4096);
        if (sym < 256) {
            out->data[out->len++] = (char)sym;
            continue;
        }
        if (sym == 256)
            return 1;
        sym -= 257;
        if (sym >= 29)
            return 0;
        len = k_len_base[sym] + (int)take(b, k_len_extra[sym]);
        sym = decode(b, dc);
        if (sym < 0 || sym >= 30)
            return 0;
        dist = k_dist_base[sym] + (int)take(b, k_dist_extra[sym]);
        if (b->error)
            return 0;
        made = out->len - start;
        back = (size_t)dist > made ? (size_t)dist - made : 0; /* how far into earlier messages */
        if (back > WINDOW || back > z->total)
            return 0;
        /* The bytes still in earlier messages, then the ones this message made. */
        for (; len && (size_t)dist > made; len--, made++)
            out->data[out->len++] = (char)z->window[(z->total - ((size_t)dist - made)) & (WINDOW - 1)];
        for (; len; len--, out->len++)
            out->data[out->len] = out->data[out->len - (size_t)dist];
    }
}

static int stored(bits_t *b, sb_t *out)
{
    unsigned len, nlen;

    /* To the byte boundary: the rest of the current byte is dropped, the whole bytes read ahead given back. */
    b->p -= b->count / 8;
    b->bits = 0;
    b->count = 0;
    if (b->end - b->p < 4)
        return 0;
    len = b->p[0] | (unsigned)b->p[1] << 8;
    nlen = b->p[2] | (unsigned)b->p[3] << 8;
    b->p += 4;
    if (len != (~nlen & 0xFFFFu) || (size_t)(b->end - b->p) < len)
        return 0;
    sb_addn(out, (const char *)b->p, len);
    b->p += len;
    return 1;
}

static int dynamic(inflate_t *z, bits_t *b, sb_t *out, size_t start)
{
    static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned char lengths[320];
    int nlen = (int)take(b, 5) + 257, ndist = (int)take(b, 5) + 1, ncode = (int)take(b, 4) + 4, index = 0;

    if (b->error || nlen > 286 || ndist > 30)
        return 0;
    memset(lengths, 0, 19);
    for (int k = 0; k < ncode; k++)
        lengths[order[k]] = (unsigned char)take(b, 3);
    if (!build(&z->lencode, lengths, 19))
        return 0;
    while (index < nlen + ndist) {
        int sym = decode(b, &z->lencode), len = 0, rep;
        if (sym < 0)
            return 0;
        if (sym < 16) {
            lengths[index++] = (unsigned char)sym;
            continue;
        }
        if (sym == 16) {
            if (!index)
                return 0;
            len = lengths[index - 1];
            rep = 3 + (int)take(b, 2);
        } else if (sym == 17) {
            rep = 3 + (int)take(b, 3);
        } else {
            rep = 11 + (int)take(b, 7);
        }
        if (b->error || index + rep > nlen + ndist)
            return 0;
        while (rep--)
            lengths[index++] = (unsigned char)len;
    }
    if (!lengths[256] || !build(&z->lencode, lengths, nlen) || !build(&z->distcode, lengths + nlen, ndist))
        return 0;
    return codes(z, b, &z->lencode, &z->distcode, out, start);
}

static void fixed_codes(inflate_t *z)
{
    unsigned char lengths[288];
    int s;

    for (s = 0; s < 144; s++)
        lengths[s] = 8;
    for (; s < 256; s++)
        lengths[s] = 9;
    for (; s < 280; s++)
        lengths[s] = 7;
    for (; s < 288; s++)
        lengths[s] = 8;
    build(&z->fixed_len, lengths, 288);
    for (s = 0; s < 30; s++)
        lengths[s] = 5;
    build(&z->fixed_dist, lengths, 30);
    z->fixed_ready = 1;
}

void inflate_init(inflate_t *z)
{
    z->started = z->done = 0;
    z->total = 0;
}

int inflate_complete(const unsigned char *in, size_t n)
{
    return n >= 4 && in[n - 4] == 0 && in[n - 3] == 0 && in[n - 2] == 0xFF && in[n - 1] == 0xFF;
}

int inflate_message(inflate_t *z, const unsigned char *in, size_t n, sb_t *out)
{
    bits_t b = {in, in + n, 0, 0, 0};
    size_t start = out->len, made, keep;

    if (!z->fixed_ready)
        fixed_codes(z);
    if (!z->started) {
        if (n < 2 || (in[0] & 15) != 8 || ((unsigned)in[0] << 8 | in[1]) % 31)
            return 0;
        b.p += 2;
        z->started = 1;
    }
    while (!z->done) {
        int last, type, ok;
        refill(&b);
        if (b.p == b.end && b.count < 8)
            break; /* this message's blocks are done (a block takes more than 7 bits) */
        last = (int)take(&b, 1);
        type = (int)take(&b, 2);
        ok = type == 0   ? stored(&b, out)
             : type == 1 ? codes(z, &b, &z->fixed_len, &z->fixed_dist, out, start)
             : type == 2 ? dynamic(z, &b, out, start)
                         : 0;
        if (!ok || b.error)
            return 0;
        z->done = last;
    }
    /* Keep the tail for the next message's matches: the window is a ring, filled in up to two copies. */
    made = out->len - start;
    keep = made > WINDOW ? WINDOW : made;
    if (keep) {
        size_t at = (size_t)((z->total + made - keep) & (WINDOW - 1)), first = WINDOW - at < keep ? WINDOW - at : keep;
        memcpy(z->window + at, out->data + out->len - keep, first);
        memcpy(z->window, out->data + out->len - keep + first, keep - first);
    }
    z->total += made;
    if (out->data)
        out->data[out->len] = 0;
    return 1;
}
