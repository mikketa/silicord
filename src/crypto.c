#include <windows.h>
#include <bcrypt.h>
#include "crypto.h"
#include "mem.h"

struct rsa_key {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_KEY_HANDLE key;
};

void sha256(const void *data, size_t n, unsigned char out[32])
{
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, (ULONG)n, out, 32);
}

rsa_key_t *rsa_generate(void)
{
    rsa_key_t *k = mem_alloc(sizeof *k);

    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&k->alg, BCRYPT_RSA_ALGORITHM, NULL, 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateKeyPair(k->alg, &k->key, 2048, 0)) &&
        BCRYPT_SUCCESS(BCryptFinalizeKeyPair(k->key, 0)))
        return k;
    rsa_free(k);
    return NULL;
}

void rsa_free(rsa_key_t *k)
{
    if (!k)
        return;
    if (k->key)
        BCryptDestroyKey(k->key);
    if (k->alg)
        BCryptCloseAlgorithmProvider(k->alg, 0);
    mem_free(k);
}

static void der_header(sb_t *out, unsigned char tag, size_t len)
{
    unsigned char h[4] = {tag};
    size_t n;

    if (len < 0x80) {
        h[1] = (unsigned char)len;
        n = 2;
    } else if (len < 0x100) {
        h[1] = 0x81;
        h[2] = (unsigned char)len;
        n = 3;
    } else {
        h[1] = 0x82;
        h[2] = (unsigned char)(len >> 8);
        h[3] = (unsigned char)len;
        n = 4;
    }
    sb_addn(out, (const char *)h, n);
}

/* Big-endian unsigned integer as a DER INTEGER. */
static void der_uint(sb_t *out, const unsigned char *p, size_t n)
{
    while (n > 1 && !*p) {
        p++;
        n--;
    }
    if (*p & 0x80) {
        der_header(out, 0x02, n + 1);
        sb_addn(out, "", 1);
    } else {
        der_header(out, 0x02, n);
    }
    sb_addn(out, (const char *)p, n);
}

int rsa_public_spki(rsa_key_t *k, sb_t *der)
{
    static const unsigned char rsa_algorithm[] = {
        0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00,
    };
    ULONG size = 0;
    BCRYPT_RSAKEY_BLOB *blob;
    const unsigned char *exp, *mod;
    sb_t ints = {0}, rsa_pub = {0}, spki = {0};

    if (!BCRYPT_SUCCESS(BCryptExportKey(k->key, NULL, BCRYPT_RSAPUBLIC_BLOB, NULL, 0, &size, 0)))
        return 0;
    blob = mem_alloc(size);
    if (!BCRYPT_SUCCESS(BCryptExportKey(k->key, NULL, BCRYPT_RSAPUBLIC_BLOB, (PUCHAR)blob, size, &size, 0))) {
        mem_free(blob);
        return 0;
    }
    exp = (const unsigned char *)(blob + 1);
    mod = exp + blob->cbPublicExp;

    /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
    der_uint(&ints, mod, blob->cbModulus);
    der_uint(&ints, exp, blob->cbPublicExp);
    der_header(&rsa_pub, 0x30, ints.len);
    sb_addn(&rsa_pub, ints.data, ints.len);

    /* SubjectPublicKeyInfo ::= SEQUENCE { algorithm, subjectPublicKey BIT STRING } */
    sb_addn(&spki, (const char *)rsa_algorithm, sizeof rsa_algorithm);
    der_header(&spki, 0x03, rsa_pub.len + 1);
    sb_addn(&spki, "", 1); /* no unused bits */
    sb_addn(&spki, rsa_pub.data, rsa_pub.len);
    der_header(der, 0x30, spki.len);
    sb_addn(der, spki.data, spki.len);

    sb_free(&spki);
    sb_free(&rsa_pub);
    sb_free(&ints);
    mem_free(blob);
    return 1;
}

static int oaep(rsa_key_t *k, const void *in, size_t n, sb_t *out, int encrypt)
{
    BCRYPT_OAEP_PADDING_INFO pad = {BCRYPT_SHA256_ALGORITHM, NULL, 0};
    ULONG size = 0;
    NTSTATUS st;

    st = encrypt ? BCryptEncrypt(k->key, (PUCHAR)in, (ULONG)n, &pad, NULL, 0, NULL, 0, &size, BCRYPT_PAD_OAEP)
                 : BCryptDecrypt(k->key, (PUCHAR)in, (ULONG)n, &pad, NULL, 0, NULL, 0, &size, BCRYPT_PAD_OAEP);
    if (!BCRYPT_SUCCESS(st))
        return 0;
    sb_reserve(out, size);
    st = encrypt ? BCryptEncrypt(k->key, (PUCHAR)in, (ULONG)n, &pad, NULL, 0,
                                 (PUCHAR)out->data + out->len, size, &size, BCRYPT_PAD_OAEP)
                 : BCryptDecrypt(k->key, (PUCHAR)in, (ULONG)n, &pad, NULL, 0,
                                 (PUCHAR)out->data + out->len, size, &size, BCRYPT_PAD_OAEP);
    if (!BCRYPT_SUCCESS(st))
        return 0;
    out->len += size;
    out->data[out->len] = 0;
    return 1;
}

int rsa_encrypt_oaep_sha256(rsa_key_t *k, const void *in, size_t n, sb_t *out)
{
    return oaep(k, in, n, out, 1);
}

int rsa_decrypt_oaep_sha256(rsa_key_t *k, const void *in, size_t n, sb_t *out)
{
    return oaep(k, in, n, out, 0);
}
