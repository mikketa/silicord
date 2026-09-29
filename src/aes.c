#include <string.h>
#include <emmintrin.h>
#include <wmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
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

static u64 load64(const unsigned char *p)
{
    u64 v = 0;

    for (int i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

static void store64(unsigned char *p, u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (56 - 8 * i));
}

/* x * h in GCM's bit-reflected GF(2^128), both as big-endian halves, with masks instead of branches. */
static void gf_mult(u64 x[2], const u64 h[2])
{
    u64 zh = 0, zl = 0, vh = h[0], vl = h[1];

    for (int w = 0; w < 2; w++) {
        u64 bits = x[w];
        for (int i = 0; i < 64; i++, bits <<= 1) {
            u64 mask = 0 - (bits >> 63), lsb = 0 - (vl & 1);
            zh ^= vh & mask;
            zl ^= vl & mask;
            vl = vl >> 1 | vh << 63;
            vh = vh >> 1 ^ (0xE1ull << 56 & lsb);
        }
    }
    x[0] = zh;
    x[1] = zl;
}

static void ghash_blocks(u64 x[2], const u64 h[2], const unsigned char *p, size_t n)
{
    while (n) {
        unsigned char block[16] = {0};
        size_t take = n < 16 ? n : 16;
        memcpy(block, p, take);
        x[0] ^= load64(block);
        x[1] ^= load64(block + 8);
        gf_mult(x, h);
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
    unsigned char h[16] = {0}, j0[16], s[16];
    u64 hk[2], x[2] = {0, 0};

    aes_encrypt_block(a, h, h);
    hk[0] = load64(h);
    hk[1] = load64(h + 8);
    ghash_blocks(x, hk, aad, an);
    ghash_blocks(x, hk, ct, n);
    x[0] ^= (u64)an * 8; /* the lengths block, in bits */
    x[1] ^= (u64)n * 8;
    gf_mult(x, hk);
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    aes_encrypt_block(a, j0, s);
    store64(full_tag, x[0]);
    store64(full_tag + 8, x[1]);
    for (int i = 0; i < 16; i++)
        full_tag[i] ^= s[i];
    secure_wipe(h, sizeof h);
    secure_wipe(hk, sizeof hk);
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

/* ---- AES-NI and PCLMULQDQ, when the CPU has them ---- */

/*
 * Every x64 CPU since about 2010 encrypts a round and multiplies carry-less
 * in hardware, in constant time, some fifty times faster than the bit
 * planes above; those stay for the others. Build with AES_PORTABLE to test
 * them on any machine.
 */
#if defined(__GNUC__)
#define HW __attribute__((target("aes,pclmul")))
#else
#define HW
#endif

static int has_hw(void)
{
#ifdef AES_PORTABLE
    return 0;
#else
    static volatile int known; /* 0 not asked yet, 1 no, 2 yes: racing threads find the same */
    if (!known) {
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, 1);
        known = (r[2] & (1 << 25)) && (r[2] & (1 << 1)) ? 2 : 1;
#else
        unsigned a, b, c, d;
        known = __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 25)) && (c & (1u << 1)) ? 2 : 1;
#endif
    }
    return known == 2;
#endif
}

/* One step of the key schedule: k's words xored with the ones before them, then with t's chosen word. */
static HW __m128i expand_step(__m128i k, __m128i t)
{
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    return _mm_xor_si128(_mm_xor_si128(k, _mm_slli_si128(k, 4)), t);
}

/* Round key i from the one before: RotWord and SubWord of its last word, xored with rcon. */
#define EXPAND128(i, rcon)                                                                                           \
    rk[i] = expand_step(rk[i - 1], _mm_shuffle_epi32(_mm_aeskeygenassist_si128(rk[i - 1], rcon), 0xFF))
/* AES-256 alternates: the same from two keys back, then SubWord alone of the last word. */
#define EXPAND256_EVEN(i, rcon)                                                                                      \
    rk[i] = expand_step(rk[i - 2], _mm_shuffle_epi32(_mm_aeskeygenassist_si128(rk[i - 1], rcon), 0xFF))
#define EXPAND256_ODD(i)                                                                                             \
    rk[i] = expand_step(rk[i - 2], _mm_shuffle_epi32(_mm_aeskeygenassist_si128(rk[i - 1], 0), 0xAA))

/* The round keys of a 16- or 32-byte key; returns the rounds. */
static HW int hw_keys(const unsigned char *key, size_t n, __m128i rk[15])
{
    rk[0] = _mm_loadu_si128((const __m128i *)key);
    if (n == 16) {
        EXPAND128(1, 0x01);
        EXPAND128(2, 0x02);
        EXPAND128(3, 0x04);
        EXPAND128(4, 0x08);
        EXPAND128(5, 0x10);
        EXPAND128(6, 0x20);
        EXPAND128(7, 0x40);
        EXPAND128(8, 0x80);
        EXPAND128(9, 0x1B);
        EXPAND128(10, 0x36);
        return 10;
    }
    rk[1] = _mm_loadu_si128((const __m128i *)(key + 16));
    EXPAND256_EVEN(2, 0x01);
    EXPAND256_ODD(3);
    EXPAND256_EVEN(4, 0x02);
    EXPAND256_ODD(5);
    EXPAND256_EVEN(6, 0x04);
    EXPAND256_ODD(7);
    EXPAND256_EVEN(8, 0x08);
    EXPAND256_ODD(9);
    EXPAND256_EVEN(10, 0x10);
    EXPAND256_ODD(11);
    EXPAND256_EVEN(12, 0x20);
    EXPAND256_ODD(13);
    EXPAND256_EVEN(14, 0x40);
    return 14;
}

static HW __m128i hw_encrypt(const __m128i *rk, int rounds, __m128i x)
{
    x = _mm_xor_si128(x, rk[0]);
    for (int r = 1; r < rounds; r++)
        x = _mm_aesenc_si128(x, rk[r]);
    return _mm_aesenclast_si128(x, rk[rounds]);
}

/* The 16 bytes in reverse order, with SSE2 alone: dwords, then words, then bytes. */
static __m128i reverse16(__m128i x)
{
    x = _mm_shuffle_epi32(x, 0x1B);
    x = _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, 0xB1), 0xB1);
    return _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8));
}

/*
 * a * b in GCM's field, both byte-reversed: the carry-less product, shifted
 * one bit left for GCM's reflected bit order, then reduced modulo
 * x^128 + x^7 + x^2 + x + 1 (Gueron and Kounavis, Intel white paper on
 * carry-less multiplication, algorithm 5).
 */
static HW __m128i gf_mult_hw(__m128i a, __m128i b)
{
    __m128i lo = _mm_clmulepi64_si128(a, b, 0x00), hi = _mm_clmulepi64_si128(a, b, 0x11);
    __m128i mid = _mm_xor_si128(_mm_clmulepi64_si128(a, b, 0x10), _mm_clmulepi64_si128(a, b, 0x01));
    __m128i c7, c8, c9;

    lo = _mm_xor_si128(lo, _mm_slli_si128(mid, 8));
    hi = _mm_xor_si128(hi, _mm_srli_si128(mid, 8));
    /* The 256-bit product one bit to the left. */
    c7 = _mm_srli_epi32(lo, 31);
    c8 = _mm_srli_epi32(hi, 31);
    lo = _mm_slli_epi32(lo, 1);
    hi = _mm_slli_epi32(hi, 1);
    c9 = _mm_srli_si128(c7, 12);
    c8 = _mm_slli_si128(c8, 4);
    c7 = _mm_slli_si128(c7, 4);
    lo = _mm_or_si128(lo, c7);
    hi = _mm_or_si128(_mm_or_si128(hi, c8), c9);
    /* Reduction. */
    c7 = _mm_xor_si128(_mm_xor_si128(_mm_slli_epi32(lo, 31), _mm_slli_epi32(lo, 30)), _mm_slli_epi32(lo, 25));
    c8 = _mm_srli_si128(c7, 4);
    lo = _mm_xor_si128(lo, _mm_slli_si128(c7, 12));
    c9 = _mm_xor_si128(_mm_xor_si128(_mm_srli_epi32(lo, 1), _mm_srli_epi32(lo, 2)), _mm_srli_epi32(lo, 7));
    return _mm_xor_si128(hi, _mm_xor_si128(lo, _mm_xor_si128(c9, c8)));
}

static HW __m128i hw_ghash(__m128i x, __m128i h, const unsigned char *p, size_t n)
{
    while (n) {
        unsigned char block[16] = {0};
        size_t take = n < 16 ? n : 16;
        memcpy(block, p, take);
        x = gf_mult_hw(_mm_xor_si128(x, reverse16(_mm_loadu_si128((const __m128i *)block))), h);
        p += take;
        n -= take;
    }
    return x;
}

/* GCM's tag over aad and the ciphertext ct. */
static HW void hw_tag(const __m128i *rk, int rounds, const unsigned char nonce[12], const void *aad, size_t an,
                      const unsigned char *ct, size_t n, unsigned char full_tag[16])
{
    unsigned char j0[16], len[16];
    __m128i h = reverse16(hw_encrypt(rk, rounds, _mm_setzero_si128())), x = _mm_setzero_si128();

    x = hw_ghash(x, h, aad, an);
    x = hw_ghash(x, h, ct, n);
    store64(len, (u64)an * 8); /* the lengths block, in bits */
    store64(len + 8, (u64)n * 8);
    x = gf_mult_hw(_mm_xor_si128(x, reverse16(_mm_loadu_si128((const __m128i *)len))), h);
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    _mm_storeu_si128((__m128i *)full_tag,
                     _mm_xor_si128(reverse16(x), hw_encrypt(rk, rounds, _mm_loadu_si128((const __m128i *)j0))));
    secure_wipe(&h, sizeof h);
}

/* Counter mode from the counter after J0, four blocks at a time. */
static HW void hw_ctr(const __m128i *rk, int rounds, const unsigned char nonce[12], const unsigned char *in,
                      unsigned char *out, size_t n)
{
    unsigned char ctr[16], ks[64];

    memcpy(ctr, nonce, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    for (size_t off = 0; off < n; off += 64) {
        __m128i b[4];
        size_t take = n - off < 64 ? n - off : 64;
        for (int k = 0; k < 4; k++) {
            inc32(ctr);
            b[k] = _mm_xor_si128(_mm_loadu_si128((const __m128i *)ctr), rk[0]);
        }
        for (int r = 1; r < rounds; r++)
            for (int k = 0; k < 4; k++)
                b[k] = _mm_aesenc_si128(b[k], rk[r]);
        for (int k = 0; k < 4; k++)
            _mm_storeu_si128((__m128i *)(ks + 16 * k), _mm_aesenclast_si128(b[k], rk[rounds]));
        for (size_t i = 0; i < take; i++)
            out[off + i] = (unsigned char)(in[off + i] ^ ks[i]);
    }
    secure_wipe(ks, sizeof ks);
}

static HW int hw_seal(const unsigned char *key, size_t kn, const unsigned char nonce[12], const void *aad, size_t an,
                      const void *in, size_t n, void *out, unsigned char *tag, size_t tag_n)
{
    __m128i rk[15];
    unsigned char full[16];
    int rounds = hw_keys(key, kn, rk);

    hw_ctr(rk, rounds, nonce, in, out, n);
    hw_tag(rk, rounds, nonce, aad, an, out, n, full);
    memcpy(tag, full, tag_n);
    secure_wipe(rk, sizeof rk);
    return 1;
}

static HW int hw_open(const unsigned char *key, size_t kn, const unsigned char nonce[12], const void *aad, size_t an,
                      const void *in, size_t n, void *out, const unsigned char *tag, size_t tag_n)
{
    __m128i rk[15];
    unsigned char full[16];
    int rounds = hw_keys(key, kn, rk), ok;

    hw_tag(rk, rounds, nonce, aad, an, in, n, full);
    ok = ct_equal(full, tag, tag_n);
    if (ok)
        hw_ctr(rk, rounds, nonce, in, out, n);
    secure_wipe(rk, sizeof rk);
    return ok;
}

/* ---- Sealing and opening ---- */

int aes_gcm_seal(const unsigned char *key, size_t kn, const unsigned char nonce[12], const void *aad, size_t an,
                 const void *in, size_t n, void *out, unsigned char *tag, size_t tag_n)
{
    aes_t a;
    unsigned char full[16];

    if (tag_n > 16 || (kn != 16 && kn != 32))
        return 0;
    if (has_hw())
        return hw_seal(key, kn, nonce, aad, an, in, n, out, tag, tag_n);
    if (!aes_init(&a, key, kn))
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

    if (tag_n > 16 || tag_n < 4 || (kn != 16 && kn != 32))
        return 0;
    if (has_hw())
        return hw_open(key, kn, nonce, aad, an, in, n, out, tag, tag_n);
    if (!aes_init(&a, key, kn))
        return 0;
    gcm(&a, nonce, aad, an, in, n, full);
    ok = ct_equal(full, tag, tag_n);
    if (ok)
        ctr_xor(&a, nonce, in, out, n);
    secure_wipe(&a, sizeof a);
    return ok;
}
