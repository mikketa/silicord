#include <string.h>
#include "aes.h"
#include "sha2.h"

typedef unsigned long long u64;

/* ---- Bit planes, and the S-box without branches or tables ---- */

/*
 * Boyar and Peralta's circuit for the S-box (IACR ePrint 2009/191), 32 ANDs
 * and 83 XORs, on bit planes: q[j] holds bit j of every byte.
 */
static void sbox_planes(unsigned q[8])
{
    unsigned x0 = q[7], x1 = q[6], x2 = q[5], x3 = q[4], x4 = q[3], x5 = q[2], x6 = q[1], x7 = q[0];
    unsigned y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, y11, y12, y13, y14, y15, y16, y17, y18, y19, y20, y21;
    unsigned t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21,
        t22, t23, t24, t25, t26, t27, t28, t29, t30, t31, t32, t33, t34, t35, t36, t37, t38, t39, t40, t41, t42,
        t43, t44, t45, t46, t47, t48, t49, t50, t51, t52, t53, t54, t55, t56, t57, t58, t59, t60, t61, t62, t63,
        t64, t65, t66, t67;
    unsigned z0, z1, z2, z3, z4, z5, z6, z7, z8, z9, z10, z11, z12, z13, z14, z15, z16, z17;

    /* Top linear transform. */
    y14 = x3 ^ x5;
    y13 = x0 ^ x6;
    y9 = x0 ^ x3;
    y8 = x0 ^ x5;
    t0 = x1 ^ x2;
    y1 = t0 ^ x7;
    y4 = y1 ^ x3;
    y12 = y13 ^ y14;
    y2 = y1 ^ x0;
    y5 = y1 ^ x6;
    y3 = y5 ^ y8;
    t1 = x4 ^ y12;
    y15 = t1 ^ x5;
    y20 = t1 ^ x1;
    y6 = y15 ^ x7;
    y10 = y15 ^ t0;
    y11 = y20 ^ y9;
    y7 = x7 ^ y11;
    y17 = y10 ^ y11;
    y19 = y10 ^ y8;
    y16 = t0 ^ y11;
    y21 = y13 ^ y16;
    y18 = x0 ^ y16;

    /* The inversion in GF(2^8). */
    t2 = y12 & y15;
    t3 = y3 & y6;
    t4 = t3 ^ t2;
    t5 = y4 & x7;
    t6 = t5 ^ t2;
    t7 = y13 & y16;
    t8 = y5 & y1;
    t9 = t8 ^ t7;
    t10 = y2 & y7;
    t11 = t10 ^ t7;
    t12 = y9 & y11;
    t13 = y14 & y17;
    t14 = t13 ^ t12;
    t15 = y8 & y10;
    t16 = t15 ^ t12;
    t17 = t4 ^ t14;
    t18 = t6 ^ t16;
    t19 = t9 ^ t14;
    t20 = t11 ^ t16;
    t21 = t17 ^ y20;
    t22 = t18 ^ y19;
    t23 = t19 ^ y21;
    t24 = t20 ^ y18;
    t25 = t21 ^ t22;
    t26 = t21 & t23;
    t27 = t24 ^ t26;
    t28 = t25 & t27;
    t29 = t28 ^ t22;
    t30 = t23 ^ t24;
    t31 = t22 ^ t26;
    t32 = t31 & t30;
    t33 = t32 ^ t24;
    t34 = t23 ^ t33;
    t35 = t27 ^ t33;
    t36 = t24 & t35;
    t37 = t36 ^ t34;
    t38 = t27 ^ t36;
    t39 = t29 & t38;
    t40 = t25 ^ t39;
    t41 = t40 ^ t37;
    t42 = t29 ^ t33;
    t43 = t29 ^ t40;
    t44 = t33 ^ t37;
    t45 = t42 ^ t41;
    z0 = t44 & y15;
    z1 = t37 & y6;
    z2 = t33 & x7;
    z3 = t43 & y16;
    z4 = t40 & y1;
    z5 = t29 & y7;
    z6 = t42 & y11;
    z7 = t45 & y17;
    z8 = t41 & y10;
    z9 = t44 & y12;
    z10 = t37 & y3;
    z11 = t33 & y4;
    z12 = t43 & y13;
    z13 = t40 & y5;
    z14 = t29 & y2;
    z15 = t42 & y9;
    z16 = t45 & y14;
    z17 = t41 & y8;

    /* Bottom linear transform, the affine constant 0x63 included. */
    t46 = z15 ^ z16;
    t47 = z10 ^ z11;
    t48 = z5 ^ z13;
    t49 = z9 ^ z10;
    t50 = z2 ^ z12;
    t51 = z2 ^ z5;
    t52 = z7 ^ z8;
    t53 = z0 ^ z3;
    t54 = z6 ^ z7;
    t55 = z16 ^ z17;
    t56 = z12 ^ t48;
    t57 = t50 ^ t53;
    t58 = z4 ^ t46;
    t59 = z3 ^ t54;
    t60 = t46 ^ t57;
    t61 = z14 ^ t57;
    t62 = t52 ^ t58;
    t63 = t49 ^ t58;
    t64 = z4 ^ t59;
    t65 = t61 ^ t62;
    t66 = z1 ^ t63;
    t67 = t64 ^ t65;
    q[7] = t59 ^ t63;
    q[1] = t56 ^ ~t62;
    q[0] = t48 ^ ~t60;
    q[4] = t53 ^ t66;
    q[3] = t51 ^ t66;
    q[2] = t47 ^ t65;
    q[6] = t64 ^ ~q[4];
    q[5] = t55 ^ ~t67;
}

/* An 8x8 bit matrix, a row per byte, transposed (Hacker's Delight, 7-3). */
static u64 transpose8(u64 x)
{
    u64 t;

    t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAull;
    x ^= t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCull;
    x ^= t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ull;
    x ^= t ^ (t << 28);
    return x;
}

/* 16 bytes into bit planes: bit i of q[j] is bit j of byte i. */
static void to_planes(const unsigned char b[16], unsigned q[8])
{
    u64 lo = 0, hi = 0;

    for (int i = 7; i >= 0; i--) {
        lo = lo << 8 | b[i];
        hi = hi << 8 | b[8 + i];
    }
    lo = transpose8(lo);
    hi = transpose8(hi);
    for (int j = 0; j < 8; j++)
        q[j] = (unsigned)(lo >> (8 * j) & 0xFF) | (unsigned)(hi >> (8 * j) & 0xFF) << 8;
}

static void from_planes(const unsigned q[8], unsigned char b[16])
{
    u64 lo = 0, hi = 0;

    for (int j = 7; j >= 0; j--) {
        lo = lo << 8 | (q[j] & 0xFF);
        hi = hi << 8 | (q[j] >> 8 & 0xFF);
    }
    lo = transpose8(lo);
    hi = transpose8(hi);
    for (int i = 0; i < 8; i++) {
        b[i] = (unsigned char)(lo >> (8 * i));
        b[8 + i] = (unsigned char)(hi >> (8 * i));
    }
}

/* ---- AES, on bit planes ---- */

/*
 * The state is column-major (byte c * 4 + row), so a plane holds a column per
 * nibble; rot1 and rot2 give row r of every column its row r + 1 or r + 2.
 */
static unsigned rot1(unsigned x)
{
    return (x >> 1 & 0x7777) | (x << 3 & 0x8888);
}

static unsigned rot2(unsigned x)
{
    return (x >> 2 & 0x3333) | (x << 2 & 0xCCCC);
}

/* Row r moves r columns left; this also clears the bits above 16 that the S-box's NOTs set. */
static void shift_rows(unsigned q[8])
{
    for (int j = 0; j < 8; j++) {
        unsigned x = q[j] & 0xFFFF;
        q[j] = (x & 0x1111) | ((x >> 4 | x << 12) & 0x2222) | ((x >> 8 | x << 8) & 0x4444) |
               ((x >> 12 | x << 4) & 0x8888);
    }
}

/* a_r ^ all ^ xtime(a_r ^ a_r+1), which is a_r+1 ^ d_r+2 ^ xtime(d_r) for d_r = a_r ^ a_r+1. */
static void mix_columns(unsigned q[8])
{
    unsigned d7 = q[7] ^ rot1(q[7]), prev = 0;

    for (int j = 0; j < 8; j++) {
        unsigned r = rot1(q[j]), d = q[j] ^ r;
        /* xtime moves each bit up a plane and folds the top one into planes 0, 1, 3 and 4 (0x1B). */
        q[j] = r ^ rot2(d) ^ prev ^ ((0x1Bu >> j & 1) ? d7 : 0);
        prev = d;
    }
}

int aes_init(aes_t *a, const unsigned char *key, size_t n)
{
    unsigned char w[240];
    unsigned q[8];
    int nk = (int)(n / 4), words, i;
    unsigned char rcon = 1;

    if (n != 16 && n != 32)
        return 0;
    a->rounds = nk + 6;
    words = 4 * (a->rounds + 1);
    memcpy(w, key, n);
    for (i = nk; i < words; i++) {
        unsigned char t[16] = {0};
        memcpy(t, w + (i - 1) * 4, 4);
        if (i % nk == 0 || (nk > 6 && i % nk == 4)) { /* SubWord */
            to_planes(t, q);
            sbox_planes(q);
            from_planes(q, t);
        }
        if (i % nk == 0) { /* RotWord, which commutes with SubWord */
            unsigned char first = t[0];
            t[0] = (unsigned char)(t[1] ^ rcon);
            t[1] = t[2];
            t[2] = t[3];
            t[3] = first;
            rcon = (unsigned char)((rcon << 1) ^ (rcon >> 7) * 0x1B);
        }
        for (int k = 0; k < 4; k++)
            w[i * 4 + k] = (unsigned char)(w[(i - nk) * 4 + k] ^ t[k]);
        secure_wipe(t, sizeof t);
    }
    for (i = 0; i <= a->rounds; i++)
        to_planes(w + 16 * i, a->rk[i]);
    secure_wipe(w, sizeof w);
    secure_wipe(q, sizeof q);
    return 1;
}

void aes_encrypt_block(const aes_t *a, const unsigned char in[16], unsigned char out[16])
{
    unsigned q[8];

    to_planes(in, q);
    for (int j = 0; j < 8; j++)
        q[j] ^= a->rk[0][j];
    for (int r = 1; r <= a->rounds; r++) {
        sbox_planes(q);
        shift_rows(q);
        if (r < a->rounds)
            mix_columns(q);
        for (int j = 0; j < 8; j++)
            q[j] ^= a->rk[r][j];
    }
    from_planes(q, out);
    secure_wipe(q, sizeof q);
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
