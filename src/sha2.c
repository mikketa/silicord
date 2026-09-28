#include <string.h>
#include "sha2.h"

static const unsigned k_round[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static unsigned ror(unsigned x, int n)
{
    return x >> n | x << (32 - n);
}

static void compress(unsigned s[8], const unsigned char *p)
{
    unsigned w[64], a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];

    for (int i = 0; i < 16; i++)
        w[i] = (unsigned)p[i * 4] << 24 | (unsigned)p[i * 4 + 1] << 16 | (unsigned)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        unsigned s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        unsigned t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k_round[i] + w[i];
        unsigned t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s[0] += a, s[1] += b, s[2] += c, s[3] += d, s[4] += e, s[5] += f, s[6] += g, s[7] += h;
    secure_wipe(w, sizeof w);
}

void sha256_init(sha256_t *h)
{
    static const unsigned iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    memcpy(h->state, iv, sizeof iv);
    h->total = 0;
    h->used = 0;
}

void sha256_update(sha256_t *h, const void *data, size_t n)
{
    const unsigned char *p = data;

    h->total += n;
    while (n) {
        size_t take = 64 - h->used < n ? 64 - h->used : n;
        memcpy(h->block + h->used, p, take);
        h->used += take;
        p += take;
        n -= take;
        if (h->used == 64) {
            compress(h->state, h->block);
            h->used = 0;
        }
    }
}

void sha256_final(sha256_t *h, unsigned char out[32])
{
    unsigned long long bits = h->total * 8;
    unsigned char len[8];

    for (int i = 0; i < 8; i++)
        len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(h, "\x80", 1);
    while (h->used != 56)
        sha256_update(h, "", 1);
    sha256_update(h, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(h->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h->state[i];
    }
    secure_wipe(h, sizeof *h);
}

void sha256_once(const void *data, size_t n, unsigned char out[32])
{
    sha256_t h;

    sha256_init(&h);
    sha256_update(&h, data, n);
    sha256_final(&h, out);
}

void hmac256_init(hmac256_t *m, const void *key, size_t n)
{
    unsigned char k[64] = {0}, pad[64];

    if (n > 64)
        sha256_once(key, n, k);
    else if (n)
        memcpy(k, key, n);
    for (int i = 0; i < 64; i++)
        pad[i] = (unsigned char)(k[i] ^ 0x36);
    sha256_init(&m->inner);
    sha256_update(&m->inner, pad, 64);
    for (int i = 0; i < 64; i++)
        pad[i] = (unsigned char)(k[i] ^ 0x5c);
    sha256_init(&m->outer);
    sha256_update(&m->outer, pad, 64);
    secure_wipe(k, sizeof k);
    secure_wipe(pad, sizeof pad);
}

void hmac256_update(hmac256_t *m, const void *data, size_t n)
{
    sha256_update(&m->inner, data, n);
}

void hmac256_final(hmac256_t *m, unsigned char out[32])
{
    unsigned char inner[32];

    sha256_final(&m->inner, inner);
    sha256_update(&m->outer, inner, 32);
    sha256_final(&m->outer, out);
    secure_wipe(inner, sizeof inner);
}

void hmac256(const void *key, size_t kn, const void *data, size_t n, unsigned char out[32])
{
    hmac256_t m;

    hmac256_init(&m, key, kn);
    hmac256_update(&m, data, n);
    hmac256_final(&m, out);
}

void hkdf256_extract(const void *salt, size_t sn, const void *ikm, size_t n, unsigned char prk[32])
{
    static const unsigned char zeros[32] = {0};

    /* No salt means a string of HashLen zeros. */
    hmac256(sn ? salt : zeros, sn ? sn : 32, ikm, n, prk);
}

int hkdf256_expand(const unsigned char prk[32], const void *info, size_t in, unsigned char *out, size_t n)
{
    return hkdf256_expand_n(prk, 32, info, in, out, n);
}

int hkdf256_expand_n(const unsigned char *prk, size_t pn, const void *info, size_t in, unsigned char *out, size_t n)
{
    unsigned char t[32];
    size_t done = 0;
    unsigned char counter = 1;

    if (n > 255 * 32)
        return 0;
    while (done < n) {
        hmac256_t m;
        size_t take = n - done < 32 ? n - done : 32;
        hmac256_init(&m, prk, pn);
        if (done)
            hmac256_update(&m, t, 32);
        hmac256_update(&m, info, in);
        hmac256_update(&m, &counter, 1);
        hmac256_final(&m, t);
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }
    secure_wipe(t, sizeof t);
    return 1;
}

void secure_wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;

    while (n--)
        *v++ = 0;
}

int ct_equal(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    unsigned char diff = 0;

    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}
