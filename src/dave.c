#include <string.h>
#include "dave.h"
#include "aes.h"
#include "mls_crypto.h"
#include "sha2.h"

#define TAG 8
#define MAGIC 0xFA
#define MIN_SUPPLEMENTAL (TAG + 1 + 1 + 2) /* tag, a one-byte nonce, the size, the marker */

/* ---- Key ratchet (MLS's hash ratchet) ---- */

void dave_ratchet_init(dave_ratchet_t *r, const unsigned char base_secret[16])
{
    memset(r, 0, sizeof *r);
    memcpy(r->next_secret, base_secret, 16);
    r->secret_len = 16;
}

int dave_ratchet_key(dave_ratchet_t *r, unsigned long generation, unsigned char key[16])
{
    for (int i = 0; i < r->nkeys; i++)
        if (r->gens[i] == generation) {
            memcpy(key, r->keys[i], 16);
            return 1;
        }
    if (generation < r->next_gen)
        return 0; /* erased */
    while (r->next_gen <= generation) {
        unsigned char k[16], next[32];
        int slot = r->nkeys < DAVE_KEEP ? r->nkeys++ : (int)(r->next_gen % DAVE_KEEP);
        if (!mls_derive_tree_secret(r->next_secret, r->secret_len, "key", r->next_gen, k, 16) ||
            !mls_derive_tree_secret(r->next_secret, r->secret_len, "secret", r->next_gen, next, 32))
            return 0;
        r->gens[slot] = r->next_gen;
        memcpy(r->keys[slot], k, 16);
        memcpy(r->next_secret, next, 32);
        r->secret_len = 32;
        r->next_gen++;
        secure_wipe(k, sizeof k);
        secure_wipe(next, sizeof next);
    }
    return dave_ratchet_key(r, generation, key);
}

void dave_ratchet_wipe(dave_ratchet_t *r)
{
    secure_wipe(r, sizeof *r);
}

/* ---- ULEB128 ---- */

static void uleb(sb_t *out, unsigned long long v)
{
    while (v >= 0x80) {
        char b = (char)(0x80 | (v & 0x7f));
        sb_addn(out, &b, 1);
        v >>= 7;
    }
    {
        char b = (char)v;
        sb_addn(out, &b, 1);
    }
}

/* Reads a ULEB128 value from p[*i..end); 0 when cut short or too large. */
static int read_uleb(const unsigned char *p, size_t end, size_t *i, unsigned long long *v)
{
    *v = 0;
    for (int shift = 0; *i < end && shift < 64; shift += 7) {
        unsigned char b = p[(*i)++];
        *v |= (unsigned long long)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return 1;
    }
    return 0;
}

/* ---- Frames ---- */

static void full_nonce(unsigned long nonce, unsigned char out[12])
{
    memset(out, 0, 12);
    out[8] = (unsigned char)nonce; /* the 32-bit nonce, little-endian, in the last four bytes */
    out[9] = (unsigned char)(nonce >> 8);
    out[10] = (unsigned char)(nonce >> 16);
    out[11] = (unsigned char)(nonce >> 24);
}

static int ranges_ok(const dave_range_t *ranges, int nranges, size_t n)
{
    size_t prev_end = 0;

    for (int i = 0; i < nranges; i++) {
        if (ranges[i].offset < prev_end || ranges[i].length > n || ranges[i].offset > n - ranges[i].length)
            return 0;
        prev_end = ranges[i].offset + ranges[i].length;
    }
    return 1;
}

/* The frame's unencrypted bytes (the AAD) then the others, in order, into `buf`; returns the AAD's size. */
static size_t split(const unsigned char *frame, size_t n, const dave_range_t *ranges, int nranges, sb_t *buf)
{
    size_t at = 0, an;

    for (int i = 0; i < nranges; i++)
        sb_addn(buf, (const char *)frame + ranges[i].offset, ranges[i].length);
    an = buf->len;
    for (int i = 0; i < nranges; i++) {
        sb_addn(buf, (const char *)frame + at, ranges[i].offset - at);
        at = ranges[i].offset + ranges[i].length;
    }
    sb_addn(buf, (const char *)frame + at, n - at);
    return an;
}

/* Puts the processed bytes back around the unencrypted ranges. */
static void interleave(const unsigned char *frame, size_t n, const dave_range_t *ranges, int nranges,
                       const unsigned char *processed, sb_t *out)
{
    size_t at = 0, from = 0;

    for (int i = 0; i < nranges; i++) {
        size_t gap = ranges[i].offset - at;
        sb_addn(out, (const char *)processed + from, gap);
        from += gap;
        sb_addn(out, (const char *)frame + ranges[i].offset, ranges[i].length);
        at = ranges[i].offset + ranges[i].length;
    }
    sb_addn(out, (const char *)processed + from, n - at);
}

int dave_encrypt(const unsigned char key[16], unsigned long nonce, const unsigned char *frame, size_t n,
                 const dave_range_t *ranges, int nranges, sb_t *out)
{
    unsigned char iv[12], tag[16];
    sb_t buf = {0}; /* the AAD, then the plaintext encrypted in place */
    size_t an, start = out->len;
    int ok = 0;

    if (!ranges_ok(ranges, nranges, n))
        return 0;
    an = split(frame, n, ranges, nranges, &buf);
    full_nonce(nonce, iv);
    if (aes_gcm_seal(key, 16, iv, buf.data, an, buf.data + an, buf.len - an, buf.data + an, tag, TAG)) {
        size_t supp;
        interleave(frame, n, ranges, nranges, (const unsigned char *)buf.data + an, out);
        supp = out->len;
        sb_addn(out, (const char *)tag, TAG);
        uleb(out, nonce);
        for (int i = 0; i < nranges; i++) {
            uleb(out, ranges[i].offset);
            uleb(out, ranges[i].length);
        }
        supp = out->len - supp + 3;
        if (supp <= 255) {
            unsigned char tail[3] = {(unsigned char)supp, MAGIC, MAGIC};
            sb_addn(out, (const char *)tail, 3);
            ok = 1;
        } else {
            out->len = start;
            out->data[start] = 0;
        }
    }
    sb_free(&buf);
    return ok;
}

/* The protocol frame check: the marker, a sane size, a nonce and ordered in-bounds ranges. */
static int parse(const unsigned char *in, size_t n, unsigned long *nonce, dave_range_t *ranges, int *nranges,
                 size_t *media_n)
{
    size_t supp, i, end;
    unsigned long long v;

    if (n < MIN_SUPPLEMENTAL || in[n - 1] != MAGIC || in[n - 2] != MAGIC)
        return 0;
    supp = in[n - 3];
    if (supp < MIN_SUPPLEMENTAL || supp > n)
        return 0;
    *media_n = n - supp;
    i = *media_n + TAG;
    end = n - 3;
    if (!read_uleb(in, end, &i, &v) || v > 0xffffffffull)
        return 0;
    *nonce = (unsigned long)v;
    *nranges = 0;
    while (i < end) {
        unsigned long long off, len;
        if (*nranges == 16 || !read_uleb(in, end, &i, &off) || !read_uleb(in, end, &i, &len))
            return 0;
        ranges[*nranges].offset = (size_t)off;
        ranges[*nranges].length = (size_t)len;
        (*nranges)++;
    }
    return ranges_ok(ranges, *nranges, *media_n);
}

int dave_frame_nonce(const unsigned char *in, size_t n, unsigned long *nonce)
{
    dave_range_t ranges[16];
    int nranges;
    size_t media;

    return parse(in, n, nonce, ranges, &nranges, &media);
}

int dave_decrypt(dave_ratchet_t *r, const unsigned char *in, size_t n, sb_t *out)
{
    dave_range_t ranges[16];
    int nranges, ok;
    size_t media, an;
    unsigned long nonce;
    unsigned char key[16], iv[12];
    sb_t buf = {0}; /* the AAD, then the ciphertext decrypted in place */

    if (!parse(in, n, &nonce, ranges, &nranges, &media) || !dave_ratchet_key(r, nonce >> 24, key))
        return 0;
    an = split(in, media, ranges, nranges, &buf);
    full_nonce(nonce, iv);
    ok = aes_gcm_open(key, 16, iv, buf.data, an, buf.data + an, buf.len - an, buf.data + an, in + media, TAG);
    if (ok)
        interleave(in, media, ranges, nranges, (const unsigned char *)buf.data + an, out);
    secure_wipe(key, sizeof key);
    sb_free(&buf);
    return ok;
}

int dave_is_silence(const unsigned char *in, size_t n)
{
    return n == 3 && in[0] == 0xF8 && in[1] == 0xFF && in[2] == 0xFE;
}

int dave_displayable_code(const unsigned char *data, size_t n, int len, int group, char *out, size_t out_size)
{
    int at = 0;

    if (len <= 0 || group <= 0 || group >= 8 || len % group || (size_t)len > n || (size_t)len >= out_size)
        return 0;
    for (int g = 0; g < len / group; g++) {
        unsigned long long v = 0, mod = 1;
        for (int k = 0; k < group; k++) {
            v = v << 8 | data[g * group + k];
            mod *= 10;
        }
        v %= mod;
        for (int k = group - 1; k >= 0; k--) {
            out[at + k] = (char)('0' + v % 10);
            v /= 10;
        }
        at += group;
    }
    out[at] = 0;
    return 1;
}
