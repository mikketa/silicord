#include <string.h>
#include "hpke.h"
#include "aes.h"
#include "p256.h"
#include "sc_asm.h"
#include "sha2.h"

/* Suite ids: the KEM's for the KEM, the whole suite's for the key schedule. */
static const unsigned char k_kem_suite[5] = {'K', 'E', 'M', 0x00, 0x10};
static const unsigned char k_hpke_suite[10] = {'H', 'P', 'K', 'E', 0x00, 0x10, 0x00, 0x01, 0x00, 0x01};

#define MAX_INFO 400

static void labeled_extract(const unsigned char *suite, size_t sn, const void *salt, size_t saltn, const char *label,
                            const void *ikm, size_t n, unsigned char prk[32])
{
    static const unsigned char zeros[32] = {0};
    hmac256_t m;

    hmac256_init(&m, saltn ? salt : zeros, saltn ? saltn : 32);
    hmac256_update(&m, "HPKE-v1", 7);
    hmac256_update(&m, suite, sn);
    hmac256_update(&m, label, sc_strlen(label));
    hmac256_update(&m, ikm, n);
    hmac256_final(&m, prk);
}

static int labeled_expand(const unsigned char *suite, size_t sn, const unsigned char prk[32], const char *label,
                          const void *info, size_t in, unsigned char *out, size_t len)
{
    unsigned char buf[2 + 7 + 10 + 32 + MAX_INFO];
    size_t ln = sc_strlen(label), n = 0;
    int ok;

    if (in > MAX_INFO || ln > 32)
        return 0;
    buf[n++] = (unsigned char)(len >> 8);
    buf[n++] = (unsigned char)len;
    memcpy(buf + n, "HPKE-v1", 7);
    n += 7;
    memcpy(buf + n, suite, sn);
    n += sn;
    memcpy(buf + n, label, ln);
    n += ln;
    if (in)
        memcpy(buf + n, info, in);
    n += in;
    ok = hkdf256_expand(prk, buf, n, out, len);
    secure_wipe(buf, n);
    return ok;
}

int hpke_derive_keypair(const void *ikm, size_t n, unsigned char sk[32], unsigned char pk[65])
{
    unsigned char prk[32];
    int ok = 0;

    labeled_extract(k_kem_suite, 5, NULL, 0, "dkp_prk", ikm, n, prk);
    for (unsigned counter = 0; counter < 256 && !ok; counter++) {
        unsigned char c = (unsigned char)counter;
        labeled_expand(k_kem_suite, 5, prk, "candidate", &c, 1, sk, 32);
        sk[0] &= 0xFF; /* P-256's bitmask keeps every bit */
        ok = p256_scalar_ok(sk);
    }
    secure_wipe(prk, sizeof prk);
    return ok && p256_public(sk, pk);
}

/* The KEM's shared secret from the DH output and enc || pkR. */
static void extract_and_expand(const unsigned char dh[32], const unsigned char enc[65], const unsigned char pk_r[65],
                               unsigned char shared[32])
{
    unsigned char prk[32], context[130];

    labeled_extract(k_kem_suite, 5, NULL, 0, "eae_prk", dh, 32, prk);
    memcpy(context, enc, 65);
    memcpy(context + 65, pk_r, 65);
    labeled_expand(k_kem_suite, 5, prk, "shared_secret", context, 130, shared, 32);
    secure_wipe(prk, sizeof prk);
}

static void key_schedule(const unsigned char shared[32], const void *info, size_t in, hpke_t *ctx)
{
    unsigned char ksc[65], secret[32];

    ksc[0] = 0; /* mode_base */
    labeled_extract(k_hpke_suite, 10, NULL, 0, "psk_id_hash", "", 0, ksc + 1);
    labeled_extract(k_hpke_suite, 10, NULL, 0, "info_hash", info, in, ksc + 33);
    labeled_extract(k_hpke_suite, 10, shared, 32, "secret", "", 0, secret);
    labeled_expand(k_hpke_suite, 10, secret, "key", ksc, 65, ctx->key, 16);
    labeled_expand(k_hpke_suite, 10, secret, "base_nonce", ksc, 65, ctx->base_nonce, 12);
    labeled_expand(k_hpke_suite, 10, secret, "exp", ksc, 65, ctx->exporter, 32);
    ctx->seq = 0;
    secure_wipe(secret, sizeof secret);
}

int hpke_setup_sender(const unsigned char pk_r[65], const void *info, size_t in, const unsigned char ikm_e[32],
                      unsigned char enc[65], hpke_t *ctx)
{
    unsigned char sk_e[32], dh[32], shared[32];
    int ok = hpke_derive_keypair(ikm_e, 32, sk_e, enc) && p256_ecdh(sk_e, pk_r, dh);

    if (ok) {
        extract_and_expand(dh, enc, pk_r, shared);
        key_schedule(shared, info, in, ctx);
    }
    secure_wipe(sk_e, sizeof sk_e);
    secure_wipe(dh, sizeof dh);
    secure_wipe(shared, sizeof shared);
    return ok;
}

int hpke_setup_receiver(const unsigned char enc[65], const unsigned char sk_r[32], const void *info, size_t in, hpke_t *ctx)
{
    unsigned char pk_r[65], dh[32], shared[32];
    int ok = p256_public(sk_r, pk_r) && p256_ecdh(sk_r, enc, dh);

    if (ok) {
        extract_and_expand(dh, enc, pk_r, shared);
        key_schedule(shared, info, in, ctx);
    }
    secure_wipe(dh, sizeof dh);
    secure_wipe(shared, sizeof shared);
    return ok;
}

/* base_nonce XOR the sequence number, big-endian over the last bytes. */
static void seq_nonce(const hpke_t *ctx, unsigned char nonce[12])
{
    memcpy(nonce, ctx->base_nonce, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (unsigned char)(ctx->seq >> (8 * i));
}

int hpke_seal(hpke_t *ctx, const void *aad, size_t an, const void *pt, size_t n, unsigned char *out)
{
    unsigned char nonce[12];

    seq_nonce(ctx, nonce);
    if (!aes_gcm_seal(ctx->key, 16, nonce, aad, an, pt, n, out, out + n, 16))
        return 0;
    ctx->seq++;
    return 1;
}

int hpke_open(hpke_t *ctx, const void *aad, size_t an, const void *ct, size_t n, unsigned char *out)
{
    unsigned char nonce[12];

    if (n < 16)
        return 0;
    seq_nonce(ctx, nonce);
    if (!aes_gcm_open(ctx->key, 16, nonce, aad, an, ct, n - 16, out, (const unsigned char *)ct + n - 16, 16))
        return 0;
    ctx->seq++;
    return 1;
}
