#pragma once
#include <stddef.h>

/*
 * AES-128/256 and GCM in portable, constant-time C: no table lookups indexed
 * by secrets (the state is bitsliced and the S-box a circuit), for MLS, DAVE's
 * frame encryption and the voice transport, video included.
 */
typedef struct {
    unsigned rk[15][8]; /* round keys, as bit planes */
    int rounds;         /* 10 or 14 */
} aes_t;

/* `n` is 16 or 32. Returns 0 for any other key size. */
int aes_init(aes_t *a, const unsigned char *key, size_t n);
void aes_encrypt_block(const aes_t *a, const unsigned char in[16], unsigned char out[16]);

/*
 * GCM with a 12-byte nonce; tags may be cut (DAVE sends 8 bytes). `out`
 * receives n bytes; in and out may be the same buffer.
 */
int aes_gcm_seal(const unsigned char *key, size_t kn, const unsigned char nonce[12], const void *aad, size_t an,
                 const void *in, size_t n, void *out, unsigned char *tag, size_t tag_n);
/* Returns 1 and the plaintext when the tag matches; 0 (and nothing written) otherwise. */
int aes_gcm_open(const unsigned char *key, size_t kn, const unsigned char nonce[12], const void *aad, size_t an,
                 const void *in, size_t n, void *out, const unsigned char *tag, size_t tag_n);
