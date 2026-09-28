#include <string.h>
#include "p256.h"
#include "sha2.h"

typedef unsigned int u32;
typedef unsigned long long u64;

/* 256-bit numbers as eight 32-bit limbs, least significant first. */
typedef struct {
    u32 v[8];
} fe;

/* A modulus with its Montgomery constants (R = 2^256), filled on first use. */
typedef struct {
    fe m;
    u32 m0inv; /* -m^-1 mod 2^32 */
    fe one;    /* R mod m: 1 in Montgomery form */
    fe rr;     /* R^2 mod m, to enter Montgomery form */
    fe exp;    /* m - 2, the exponent that inverts */
    int ready;
} mod_t;

static mod_t g_p = {{{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 1, 0xFFFFFFFF}}, 0, {{0}}, {{0}}, {{0}}, 0};
static mod_t g_n = {{{0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFF}},
                    0, {{0}}, {{0}}, {{0}}, 0};
static const fe k_b = {{0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0, 0x769886BC, 0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8}};
static const fe k_gx = {{0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81, 0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2}};
static const fe k_gy = {{0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357, 0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2}};

/* ---- Limbs ---- */

static u32 add8(u32 r[8], const u32 a[8], const u32 b[8])
{
    u64 c = 0;

    for (int i = 0; i < 8; i++) {
        c += (u64)a[i] + b[i];
        r[i] = (u32)c;
        c >>= 32;
    }
    return (u32)c;
}

static u32 sub8(u32 r[8], const u32 a[8], const u32 b[8])
{
    u64 borrow = 0;

    for (int i = 0; i < 8; i++) {
        u64 d = (u64)a[i] - b[i] - borrow;
        r[i] = (u32)d;
        borrow = (d >> 32) & 1;
    }
    return (u32)borrow;
}

/* r = mask ? a : r, mask being all ones or zero. */
static void cmov8(u32 r[8], const u32 a[8], u32 mask)
{
    for (int i = 0; i < 8; i++)
        r[i] = (r[i] & ~mask) | (a[i] & mask);
}

static u32 nonzero32(u32 x)
{
    return (x | (0u - x)) >> 31;
}

static u32 is_zero8(const u32 a[8])
{
    u32 acc = 0;

    for (int i = 0; i < 8; i++)
        acc |= a[i];
    return nonzero32(acc) ^ 1;
}

static void from_bytes(fe *r, const unsigned char b[32])
{
    for (int i = 0; i < 8; i++) {
        const unsigned char *p = b + 28 - 4 * i;
        r->v[i] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
    }
}

static void to_bytes(unsigned char b[32], const fe *a)
{
    for (int i = 0; i < 8; i++) {
        unsigned char *p = b + 28 - 4 * i;
        p[0] = (unsigned char)(a->v[i] >> 24);
        p[1] = (unsigned char)(a->v[i] >> 16);
        p[2] = (unsigned char)(a->v[i] >> 8);
        p[3] = (unsigned char)a->v[i];
    }
}

/* 1 when a < m. */
static u32 below(const fe *a, const fe *m)
{
    u32 d[8];

    return sub8(d, a->v, m->v);
}

/* ---- Arithmetic modulo m, inputs below m ---- */

static void fadd(fe *r, const fe *a, const fe *b, const mod_t *M)
{
    u32 s[8], d[8], c = add8(s, a->v, b->v), br = sub8(d, s, M->m.v);

    cmov8(s, d, 0u - (c | (br ^ 1)));
    memcpy(r->v, s, sizeof s);
}

static void fsub(fe *r, const fe *a, const fe *b, const mod_t *M)
{
    u32 d[8], s[8], br = sub8(d, a->v, b->v);

    add8(s, d, M->m.v);
    cmov8(d, s, 0u - br);
    memcpy(r->v, d, sizeof d);
}

/* Montgomery product a * b / R mod m (CIOS). */
static void fmul(fe *r, const fe *a, const fe *b, const mod_t *M)
{
    u32 t[10] = {0}, d[8], br;

    for (int i = 0; i < 8; i++) {
        u64 c = 0;
        u32 q;
        for (int j = 0; j < 8; j++) {
            c += (u64)t[j] + (u64)a->v[j] * b->v[i];
            t[j] = (u32)c;
            c >>= 32;
        }
        c += t[8];
        t[8] = (u32)c;
        t[9] = (u32)(c >> 32);
        q = t[0] * M->m0inv;
        c = ((u64)t[0] + (u64)q * M->m.v[0]) >> 32;
        for (int j = 1; j < 8; j++) {
            c += (u64)t[j] + (u64)q * M->m.v[j];
            t[j - 1] = (u32)c;
            c >>= 32;
        }
        c += t[8];
        t[7] = (u32)c;
        t[8] = t[9] + (u32)(c >> 32);
    }
    br = sub8(d, t, M->m.v);
    cmov8(t, d, 0u - (nonzero32(t[8]) | (br ^ 1)));
    memcpy(r->v, t, sizeof r->v);
}

static void mod_setup(mod_t *M)
{
    u32 inv = 1, zero[8] = {0};

    if (M->ready)
        return;
    for (int i = 0; i < 5; i++)
        inv *= 2u - M->m.v[0] * inv;
    M->m0inv = 0u - inv;
    sub8(M->one.v, zero, M->m.v); /* 2^256 - m, as m > 2^255 */
    M->rr = M->one;
    for (int i = 0; i < 256; i++)
        fadd(&M->rr, &M->rr, &M->rr, M);
    {
        u32 two[8] = {2};
        sub8(M->exp.v, M->m.v, two);
    }
    M->ready = 1;
}

static void to_mont(fe *r, const fe *a, const mod_t *M)
{
    fmul(r, a, &M->rr, M);
}

static void from_mont(fe *r, const fe *a, const mod_t *M)
{
    fe one = {{1}};

    fmul(r, a, &one, M);
}

/* a^(m-2): the inverse of a (Montgomery form in and out). The exponent is public. */
static void finv(fe *r, const fe *a, const mod_t *M)
{
    fe acc = M->one;

    for (int i = 255; i >= 0; i--) {
        fmul(&acc, &acc, &acc, M);
        if ((M->exp.v[i / 32] >> (i % 32)) & 1)
            fmul(&acc, &acc, a, M);
    }
    *r = acc;
}

/* ---- Points (projective X:Y:Z, Montgomery form mod p) ---- */

typedef struct {
    fe x, y, z;
} pt;

/* Complete addition for a = -3 (Renes, Costello, Batina, algorithm 4): no special cases, doubling included. */
static void padd(pt *r, const pt *p, const pt *q)
{
    const mod_t *M = &g_p;
    fe t0, t1, t2, t3, t4, x3, y3, z3, b;

    to_mont(&b, &k_b, M);
    fmul(&t0, &p->x, &q->x, M);
    fmul(&t1, &p->y, &q->y, M);
    fmul(&t2, &p->z, &q->z, M);
    fadd(&t3, &p->x, &p->y, M);
    fadd(&t4, &q->x, &q->y, M);
    fmul(&t3, &t3, &t4, M);
    fadd(&t4, &t0, &t1, M);
    fsub(&t3, &t3, &t4, M);
    fadd(&t4, &p->y, &p->z, M);
    fadd(&x3, &q->y, &q->z, M);
    fmul(&t4, &t4, &x3, M);
    fadd(&x3, &t1, &t2, M);
    fsub(&t4, &t4, &x3, M);
    fadd(&x3, &p->x, &p->z, M);
    fadd(&y3, &q->x, &q->z, M);
    fmul(&x3, &x3, &y3, M);
    fadd(&y3, &t0, &t2, M);
    fsub(&y3, &x3, &y3, M);
    fmul(&z3, &b, &t2, M);
    fsub(&x3, &y3, &z3, M);
    fadd(&z3, &x3, &x3, M);
    fadd(&x3, &x3, &z3, M);
    fsub(&z3, &t1, &x3, M);
    fadd(&x3, &t1, &x3, M);
    fmul(&y3, &b, &y3, M);
    fadd(&t1, &t2, &t2, M);
    fadd(&t2, &t1, &t2, M);
    fsub(&y3, &y3, &t2, M);
    fsub(&y3, &y3, &t0, M);
    fadd(&t1, &y3, &y3, M);
    fadd(&y3, &t1, &y3, M);
    fadd(&t1, &t0, &t0, M);
    fadd(&t0, &t1, &t0, M);
    fsub(&t0, &t0, &t2, M);
    fmul(&t1, &t4, &y3, M);
    fmul(&t2, &t0, &y3, M);
    fmul(&y3, &x3, &z3, M);
    fadd(&y3, &y3, &t2, M);
    fmul(&x3, &t3, &x3, M);
    fsub(&x3, &x3, &t1, M);
    fmul(&z3, &t4, &z3, M);
    fmul(&t1, &t3, &t0, M);
    fadd(&z3, &z3, &t1, M);
    r->x = x3;
    r->y = y3;
    r->z = z3;
}

/* k * p over all 256 bits, the same work whatever the scalar. */
static void pmul(pt *r, const pt *p, const unsigned char k[32])
{
    pt acc, t;

    memset(&acc, 0, sizeof acc);
    acc.y = g_p.one; /* the point at infinity (0 : 1 : 0) */
    for (int i = 0; i < 256; i++) {
        u32 bit = (k[i / 8] >> (7 - i % 8)) & 1, mask = 0u - bit;
        padd(&acc, &acc, &acc);
        padd(&t, &acc, p);
        cmov8(acc.x.v, t.x.v, mask);
        cmov8(acc.y.v, t.y.v, mask);
        cmov8(acc.z.v, t.z.v, mask);
    }
    *r = acc;
    secure_wipe(&t, sizeof t);
}

static void generator(pt *g)
{
    to_mont(&g->x, &k_gx, &g_p);
    to_mont(&g->y, &k_gy, &g_p);
    g->z = g_p.one;
}

/* Affine coordinates as plain numbers; 0 for the point at infinity. */
static int affine(const pt *p, fe *x, fe *y)
{
    fe zi;

    if (is_zero8(p->z.v))
        return 0;
    finv(&zi, &p->z, &g_p);
    fmul(x, &p->x, &zi, &g_p);
    fmul(y, &p->y, &zi, &g_p);
    from_mont(x, x, &g_p);
    from_mont(y, y, &g_p);
    return 1;
}

/* Decodes and checks y^2 = x^3 - 3x + b. */
static int decode(pt *p, const unsigned char pub[65])
{
    fe x, y, lhs, rhs, t, b;

    if (pub[0] != 4)
        return 0;
    from_bytes(&x, pub + 1);
    from_bytes(&y, pub + 33);
    if (!below(&x, &g_p.m) || !below(&y, &g_p.m))
        return 0;
    to_mont(&p->x, &x, &g_p);
    to_mont(&p->y, &y, &g_p);
    p->z = g_p.one;
    to_mont(&b, &k_b, &g_p);
    fmul(&lhs, &p->y, &p->y, &g_p);
    fmul(&t, &p->x, &p->x, &g_p);
    fmul(&rhs, &t, &p->x, &g_p);
    fsub(&rhs, &rhs, &p->x, &g_p);
    fsub(&rhs, &rhs, &p->x, &g_p);
    fsub(&rhs, &rhs, &p->x, &g_p);
    fadd(&rhs, &rhs, &b, &g_p);
    return ct_equal(lhs.v, rhs.v, sizeof lhs.v);
}

static void encode(unsigned char pub[65], const fe *x, const fe *y)
{
    pub[0] = 4;
    to_bytes(pub + 1, x);
    to_bytes(pub + 33, y);
}

static void setup(void)
{
    mod_setup(&g_p);
    mod_setup(&g_n);
}

/* ---- Public operations ---- */

int p256_scalar_ok(const unsigned char sk[32])
{
    fe k;

    setup();
    from_bytes(&k, sk);
    return (int)(below(&k, &g_n.m) & (is_zero8(k.v) ^ 1));
}

int p256_public(const unsigned char sk[32], unsigned char pub[65])
{
    pt g, q;
    fe x, y;

    if (!p256_scalar_ok(sk))
        return 0;
    generator(&g);
    pmul(&q, &g, sk);
    if (!affine(&q, &x, &y))
        return 0;
    encode(pub, &x, &y);
    return 1;
}

int p256_point_ok(const unsigned char pub[65])
{
    pt p;

    setup();
    return decode(&p, pub);
}

int p256_ecdh(const unsigned char sk[32], const unsigned char pub[65], unsigned char shared[32])
{
    pt p, q;
    fe x, y;

    if (!p256_scalar_ok(sk) || !decode(&p, pub))
        return 0;
    pmul(&q, &p, sk);
    if (!affine(&q, &x, &y))
        return 0;
    to_bytes(shared, &x);
    secure_wipe(&q, sizeof q);
    return 1;
}

/* A number below 2^256 reduced mod n: one subtraction is enough. */
static void reduce_n(fe *a)
{
    u32 d[8], br = sub8(d, a->v, g_n.m.v);

    cmov8(a->v, d, 0u - (br ^ 1));
}

/* RFC 6979's nonce for HMAC-SHA256 and a 256-bit order. */
static void rfc6979_k(const unsigned char sk[32], const unsigned char hash[32], unsigned char k[32])
{
    unsigned char v[32], key[32], h[32], buf[97];
    fe e, cand;

    from_bytes(&e, hash);
    reduce_n(&e);
    to_bytes(h, &e);
    memset(v, 1, 32);
    memset(key, 0, 32);
    for (unsigned char pass = 0; pass < 2; pass++) {
        memcpy(buf, v, 32);
        buf[32] = pass;
        memcpy(buf + 33, sk, 32);
        memcpy(buf + 65, h, 32);
        hmac256(key, 32, buf, 97, key);
        hmac256(key, 32, v, 32, v);
    }
    for (;;) {
        hmac256(key, 32, v, 32, v);
        from_bytes(&cand, v);
        if (below(&cand, &g_n.m) && !is_zero8(cand.v))
            break;
        memcpy(buf, v, 32);
        buf[32] = 0;
        hmac256(key, 32, buf, 33, key);
        hmac256(key, 32, v, 32, v);
    }
    memcpy(k, v, 32);
    secure_wipe(buf, sizeof buf);
    secure_wipe(key, sizeof key);
    secure_wipe(v, sizeof v);
}

int p256_sign(const unsigned char sk[32], const unsigned char hash[32], unsigned char sig[64])
{
    unsigned char k[32];
    pt g, rp;
    fe x, y, r, e, d, km, ki, t, s;
    int ok = 0;

    if (!p256_scalar_ok(sk))
        return 0;
    rfc6979_k(sk, hash, k);
    generator(&g);
    pmul(&rp, &g, k);
    if (affine(&rp, &x, &y)) {
        r = x;
        reduce_n(&r);
        from_bytes(&e, hash);
        reduce_n(&e);
        from_bytes(&d, sk);
        from_bytes(&km, k);
        /* s = k^-1 (e + r d) mod n, in Montgomery form. */
        to_mont(&km, &km, &g_n);
        finv(&ki, &km, &g_n);
        to_mont(&t, &r, &g_n);
        to_mont(&d, &d, &g_n);
        fmul(&t, &t, &d, &g_n);
        to_mont(&e, &e, &g_n);
        fadd(&t, &t, &e, &g_n);
        fmul(&s, &ki, &t, &g_n);
        from_mont(&s, &s, &g_n);
        if (!is_zero8(r.v) && !is_zero8(s.v)) {
            to_bytes(sig, &r);
            to_bytes(sig + 32, &s);
            ok = 1;
        }
    }
    secure_wipe(k, sizeof k);
    secure_wipe(&d, sizeof d);
    secure_wipe(&km, sizeof km);
    secure_wipe(&ki, sizeof ki);
    return ok;
}

int p256_verify(const unsigned char pub[65], const unsigned char hash[32], const unsigned char sig[64])
{
    pt q, g, a, b;
    fe r, s, e, w, u1, u2, x, y;
    unsigned char b1[32], b2[32];

    setup();
    if (!decode(&q, pub))
        return 0;
    from_bytes(&r, sig);
    from_bytes(&s, sig + 32);
    if (!below(&r, &g_n.m) || !below(&s, &g_n.m) || is_zero8(r.v) || is_zero8(s.v))
        return 0;
    from_bytes(&e, hash);
    reduce_n(&e);
    /* u1 = e / s, u2 = r / s; then the X of u1 G + u2 Q must be r (mod n). */
    to_mont(&w, &s, &g_n);
    finv(&w, &w, &g_n);
    to_mont(&u1, &e, &g_n);
    fmul(&u1, &u1, &w, &g_n);
    from_mont(&u1, &u1, &g_n);
    to_mont(&u2, &r, &g_n);
    fmul(&u2, &u2, &w, &g_n);
    from_mont(&u2, &u2, &g_n);
    to_bytes(b1, &u1);
    to_bytes(b2, &u2);
    generator(&g);
    pmul(&a, &g, b1);
    pmul(&b, &q, b2);
    padd(&a, &a, &b);
    if (!affine(&a, &x, &y))
        return 0;
    reduce_n(&x);
    return ct_equal(x.v, r.v, sizeof x.v);
}
