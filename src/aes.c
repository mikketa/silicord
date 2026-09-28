#include <string.h>
#include "aes.h"
#include "sha2.h"

/* ---- GF(2^8), without branches or tables ---- */

static unsigned char xtime(unsigned char a)
{
    return (unsigned char)((a << 1) ^ ((unsigned char)-(a >> 7) & 0x1b));
}

static unsigned char gmul(unsigned char a, unsigned char b)
{
    unsigned char p = 0;

    for (int i = 0; i < 8; i++) {
        p ^= (unsigned char)(-(b & 1) & a);
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

/* x^254, the inverse (and 0 for 0), by a fixed chain of products. */
static unsigned char ginv(unsigned char x)
{
    unsigned char x2 = gmul(x, x), x3 = gmul(x2, x), x6 = gmul(x3, x3), x12 = gmul(x6, x6), x15 = gmul(x12, x3);
    unsigned char x30 = gmul(x15, x15), x60 = gmul(x30, x30), x120 = gmul(x60, x60), x240 = gmul(x120, x120);

    return gmul(gmul(x240, x12), x2);
}

static unsigned char rotl8(unsigned char b, int n)
{
    return (unsigned char)(b << n | b >> (8 - n));
}

static unsigned char sbox(unsigned char x)
{
    unsigned char b = ginv(x);

    return (unsigned char)(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63);
}

/* ---- AES ---- */

int aes_init(aes_t *a, const unsigned char *key, size_t n)
{
    unsigned char w[240];
    int nk = (int)(n / 4), words, i;
    unsigned char rcon = 1;

    if (n != 16 && n != 32)
        return 0;
    a->rounds = nk + 6;
    words = 4 * (a->rounds + 1);
    memcpy(w, key, n);
    for (i = nk; i < words; i++) {
        unsigned char t[4];
        memcpy(t, w + (i - 1) * 4, 4);
        if (i % nk == 0) {
            unsigned char first = t[0];
            t[0] = (unsigned char)(sbox(t[1]) ^ rcon);
            t[1] = sbox(t[2]);
            t[2] = sbox(t[3]);
            t[3] = sbox(first);
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++)
                t[k] = sbox(t[k]);
        }
        for (int k = 0; k < 4; k++)
            w[i * 4 + k] = (unsigned char)(w[(i - nk) * 4 + k] ^ t[k]);
    }
    memcpy(a->rk, w, (size_t)words * 4);
    secure_wipe(w, sizeof w);
    return 1;
}

void aes_encrypt_block(const aes_t *a, const unsigned char in[16], unsigned char out[16])
{
    unsigned char s[16], t[16];

    for (int i = 0; i < 16; i++)
        s[i] = (unsigned char)(in[i] ^ a->rk[0][i]);
    for (int r = 1; r <= a->rounds; r++) {
        /* SubBytes and ShiftRows (the state is column-major: s[column * 4 + row]). */
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++)
                t[c * 4 + row] = sbox(s[((c + row) % 4) * 4 + row]);
        if (r < a->rounds) { /* MixColumns */
            for (int c = 0; c < 4; c++) {
                unsigned char *col = t + c * 4, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                unsigned char all = (unsigned char)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (unsigned char)(a0 ^ all ^ xtime((unsigned char)(a0 ^ a1)));
                col[1] = (unsigned char)(a1 ^ all ^ xtime((unsigned char)(a1 ^ a2)));
                col[2] = (unsigned char)(a2 ^ all ^ xtime((unsigned char)(a2 ^ a3)));
                col[3] = (unsigned char)(a3 ^ all ^ xtime((unsigned char)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++)
            s[i] = (unsigned char)(t[i] ^ a->rk[r][i]);
    }
    memcpy(out, s, 16);
    secure_wipe(s, sizeof s);
    secure_wipe(t, sizeof t);
}

/* ---- GCM ---- */

/* x * y in GCM's bit-reflected GF(2^128), with masks instead of branches. */
static void gf_mult(const unsigned char x[16], const unsigned char y[16], unsigned char out[16])
{
    unsigned char z[16] = {0}, v[16];

    memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        unsigned char mask = (unsigned char)-((x[i / 8] >> (7 - i % 8)) & 1), lsb = (unsigned char)-(v[15] & 1);
        for (int j = 0; j < 16; j++)
            z[j] ^= (unsigned char)(v[j] & mask);
        for (int j = 15; j > 0; j--)
            v[j] = (unsigned char)(v[j] >> 1 | v[j - 1] << 7);
        v[0] = (unsigned char)((v[0] >> 1) ^ (0xe1 & lsb));
    }
    memcpy(out, z, 16);
}

static void ghash_blocks(unsigned char x[16], const unsigned char h[16], const unsigned char *p, size_t n)
{
    while (n) {
        unsigned char block[16] = {0};
        size_t take = n < 16 ? n : 16;
        memcpy(block, p, take);
        for (int j = 0; j < 16; j++)
            x[j] ^= block[j];
        gf_mult(x, h, x);
        p += take;
        n -= take;
    }
}

static void inc32(unsigned char ctr[16])
{
    for (int i = 15; i >= 12 && !++ctr[i]; i--)
        ;
}

/* CTR from the counter after J0, then the tag over aad and the ciphertext. */
static void gcm(const aes_t *a, const unsigned char nonce[12], const void *aad, size_t an, const unsigned char *ct,
                size_t n, unsigned char full_tag[16])
{
    unsigned char h[16] = {0}, j0[16], s[16], x[16] = {0}, lens[16];
    unsigned long long abits = (unsigned long long)an * 8, cbits = (unsigned long long)n * 8;

    aes_encrypt_block(a, h, h);
    ghash_blocks(x, h, aad, an);
    ghash_blocks(x, h, ct, n);
    for (int i = 0; i < 8; i++) {
        lens[i] = (unsigned char)(abits >> (56 - 8 * i));
        lens[8 + i] = (unsigned char)(cbits >> (56 - 8 * i));
    }
    ghash_blocks(x, h, lens, 16);
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    aes_encrypt_block(a, j0, s);
    for (int i = 0; i < 16; i++)
        full_tag[i] = (unsigned char)(s[i] ^ x[i]);
    secure_wipe(h, sizeof h);
}

static void ctr_xor(const aes_t *a, const unsigned char nonce[12], const unsigned char *in, unsigned char *out, size_t n)
{
    unsigned char ctr[16], ks[16];

    memcpy(ctr, nonce, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    for (size_t off = 0; off < n; off += 16) {
        size_t take = n - off < 16 ? n - off : 16;
        inc32(ctr);
        aes_encrypt_block(a, ctr, ks);
        for (size_t i = 0; i < take; i++)
            out[off + i] = (unsigned char)(in[off + i] ^ ks[i]);
    }
    secure_wipe(ks, sizeof ks);
}

int aes_gcm_seal(const unsigned char *key, size_t kn, const unsigned char nonce[12], const void *aad, size_t an,
                 const void *in, size_t n, void *out, unsigned char *tag, size_t tag_n)
{
    aes_t a;
    unsigned char full[16];

    if (tag_n > 16 || !aes_init(&a, key, kn))
        return 0;
    ctr_xor(&a, nonce, in, out, n);
    gcm(&a, nonce, aad, an, out, n, full);
    memcpy(tag, full, tag_n);
    secure_wipe(&a, sizeof a);
    return 1;
}

int aes_gcm_open(const unsigned char *key, size_t kn, const unsigned char nonce[12], const void *aad, size_t an,
                 const void *in, size_t n, void *out, const unsigned char *tag, size_t tag_n)
{
    aes_t a;
    unsigned char full[16];
    int ok;

    if (tag_n > 16 || tag_n < 4 || !aes_init(&a, key, kn))
        return 0;
    gcm(&a, nonce, aad, an, in, n, full);
    ok = ct_equal(full, tag, tag_n);
    if (ok)
        ctr_xor(&a, nonce, in, out, n);
    secure_wipe(&a, sizeof a);
    return ok;
}
