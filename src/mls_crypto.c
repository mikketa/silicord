#include <string.h>
#include "mls_crypto.h"
#include "hpke.h"
#include "mem.h"
#include "p256.h"
#include "rng.h"
#include "sc_asm.h"
#include "sha2.h"
#include "tls.h"

/* opaque label<V> = "MLS 1.0 " + label */
static void add_label(sb_t *out, const char *label)
{
    size_t n = sc_strlen(label);

    tls_varint(out, 8 + n);
    sb_addn(out, "MLS 1.0 ", 8);
    sb_addn(out, label, n);
}

int mls_expand_with_label(const unsigned char *secret, size_t sn, const char *label, const void *ctx, size_t cn,
                          unsigned char *out, size_t len)
{
    sb_t info = {0};
    int ok;

    tls_u16(&info, (unsigned)len);
    add_label(&info, label);
    tls_vec(&info, ctx, cn);
    ok = hkdf256_expand_n(secret, sn, info.data, info.len, out, len);
    sb_free(&info);
    return ok;
}

int mls_derive_secret(const unsigned char secret[32], const char *label, unsigned char out[32])
{
    return mls_expand_with_label(secret, 32, label, "", 0, out, 32);
}

int mls_derive_tree_secret(const unsigned char *secret, size_t sn, const char *label, unsigned long generation,
                           unsigned char *out, size_t len)
{
    unsigned char g[4] = {(unsigned char)(generation >> 24), (unsigned char)(generation >> 16),
                          (unsigned char)(generation >> 8), (unsigned char)generation};

    return mls_expand_with_label(secret, sn, label, g, 4, out, len);
}

void mls_ref_hash(const char *label, const void *value, size_t n, unsigned char out[32])
{
    sb_t in = {0};

    tls_vec(&in, label, sc_strlen(label));
    tls_vec(&in, value, n);
    sha256_once(in.data, in.len, out);
    sb_free(&in);
}

/* SignContent's digest: label and content as vectors. */
static void sign_digest(const char *label, const void *content, size_t n, unsigned char hash[32])
{
    sb_t in = {0};

    add_label(&in, label);
    tls_vec(&in, content, n);
    sha256_once(in.data, in.len, hash);
    sb_free(&in);
}

int mls_sign_with_label(const unsigned char sk[32], const char *label, const void *content, size_t n, sb_t *sig)
{
    unsigned char hash[32], rs[64];

    sign_digest(label, content, n, hash);
    if (!p256_sign(sk, hash, rs))
        return 0;
    der_from_rs(rs, sig);
    return 1;
}

int mls_verify_with_label(const unsigned char pub[65], const char *label, const void *content, size_t n,
                          const unsigned char *sig, size_t sn)
{
    unsigned char hash[32], rs[64];

    if (!der_to_rs(sig, sn, rs))
        return 0;
    sign_digest(label, content, n, hash);
    return p256_verify(pub, hash, rs);
}

static void encrypt_context(const char *label, const void *ctx, size_t cn, sb_t *out)
{
    add_label(out, label);
    tls_vec(out, ctx, cn);
}

int mls_encrypt_with_label(const unsigned char pub[65], const char *label, const void *ctx, size_t cn, const void *pt,
                           size_t n, unsigned char kem_output[65], sb_t *ct)
{
    unsigned char ikm[32];
    sb_t info = {0};
    hpke_t h;
    int ok;

    encrypt_context(label, ctx, cn, &info);
    ok = rng_bytes(ikm, sizeof ikm) && hpke_setup_sender(pub, info.data, info.len, ikm, kem_output, &h);
    if (ok) {
        sb_reserve(ct, n + 16);
        ok = hpke_seal(&h, "", 0, pt, n, (unsigned char *)ct->data + ct->len);
        if (ok)
            ct->len += n + 16;
    }
    secure_wipe(ikm, sizeof ikm);
    secure_wipe(&h, sizeof h);
    sb_free(&info);
    return ok;
}

int mls_decrypt_with_label(const unsigned char sk[32], const char *label, const void *ctx, size_t cn,
                           const unsigned char kem_output[65], const unsigned char *ct, size_t n, sb_t *pt)
{
    sb_t info = {0};
    hpke_t h;
    int ok;

    encrypt_context(label, ctx, cn, &info);
    ok = n >= 16 && hpke_setup_receiver(kem_output, sk, info.data, info.len, &h);
    if (ok) {
        sb_reserve(pt, n - 16);
        ok = hpke_open(&h, "", 0, ct, n, (unsigned char *)pt->data + pt->len);
        if (ok)
            pt->len += n - 16;
    }
    secure_wipe(&h, sizeof h);
    sb_free(&info);
    return ok;
}

/* ---- DER ---- */

static void der_int(sb_t *out, const unsigned char v[32])
{
    int i = 0;

    while (i < 31 && !v[i])
        i++;
    tls_u8(out, 0x02);
    tls_u8(out, (unsigned)(32 - i + (v[i] >> 7)));
    if (v[i] >> 7)
        tls_u8(out, 0);
    sb_addn(out, (const char *)v + i, (size_t)(32 - i));
}

void der_from_rs(const unsigned char rs[64], sb_t *der)
{
    sb_t body = {0};

    der_int(&body, rs);
    der_int(&body, rs + 32);
    tls_u8(der, 0x30);
    tls_u8(der, (unsigned)body.len);
    sb_addn(der, body.data, body.len);
    sb_free(&body);
}

/* One INTEGER into 32 big-endian bytes. */
static int der_read_int(const unsigned char **p, const unsigned char *end, unsigned char out[32])
{
    size_t len;

    if (end - *p < 2 || (*p)[0] != 0x02)
        return 0;
    len = (*p)[1];
    *p += 2;
    if (!len || len > 33 || (size_t)(end - *p) < len)
        return 0;
    if (len == 33) { /* the sign byte */
        if ((*p)[0] != 0)
            return 0;
        (*p)++;
        len--;
    }
    memset(out, 0, 32);
    memcpy(out + 32 - len, *p, len);
    *p += len;
    return 1;
}

int der_to_rs(const unsigned char *der, size_t n, unsigned char rs[64])
{
    const unsigned char *p = der, *end;

    if (n < 8 || der[0] != 0x30 || der[1] != n - 2)
        return 0;
    p += 2;
    end = der + n;
    return der_read_int(&p, end, rs) && der_read_int(&p, end, rs + 32) && p == end;
}
