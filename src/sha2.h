#pragma once
#include <stddef.h>

/*
 * SHA-256, HMAC-SHA256 and HKDF-SHA256 (RFC 5869) in portable C, for the
 * voice encryption (MLS ciphersuite 2 and DAVE). crypto.h keeps the CNG
 * wrappers the login uses.
 */

typedef struct {
    unsigned state[8];
    unsigned long long total;  /* bytes hashed */
    unsigned char block[64];
    size_t used;
} sha256_t;

void sha256_init(sha256_t *h);
void sha256_update(sha256_t *h, const void *data, size_t n);
void sha256_final(sha256_t *h, unsigned char out[32]);
void sha256_once(const void *data, size_t n, unsigned char out[32]);

typedef struct {
    sha256_t inner, outer;
} hmac256_t;

void hmac256_init(hmac256_t *m, const void *key, size_t n);
void hmac256_update(hmac256_t *m, const void *data, size_t n);
void hmac256_final(hmac256_t *m, unsigned char out[32]);
void hmac256(const void *key, size_t kn, const void *data, size_t n, unsigned char out[32]);

void hkdf256_extract(const void *salt, size_t sn, const void *ikm, size_t n, unsigned char prk[32]);
/* Up to 255 * 32 bytes of output keying material. Returns 0 if `n` is too large. */
int hkdf256_expand(const unsigned char prk[32], const void *info, size_t in, unsigned char *out, size_t n);
/* The same with a pseudorandom key of any size (DAVE's sender ratchets start from 16 bytes). */
int hkdf256_expand_n(const unsigned char *prk, size_t pn, const void *info, size_t in, unsigned char *out, size_t n);

/* Wipes secrets so the compiler cannot drop the stores. */
void secure_wipe(void *p, size_t n);
/* Constant-time comparison: 1 when equal. */
int ct_equal(const void *a, const void *b, size_t n);
