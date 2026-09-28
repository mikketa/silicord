#pragma once
#include <stddef.h>

/*
 * HPKE (RFC 9180), base mode, with MLS ciphersuite 2's choices:
 * DHKEM(P-256, HKDF-SHA256), HKDF-SHA256 and AES-128-GCM.
 */

typedef struct {
    unsigned char key[16];
    unsigned char base_nonce[12];
    unsigned char exporter[32];
    unsigned long long seq;
} hpke_t;

/* DeriveKeyPair: the key pair an input keying material stands for. */
int hpke_derive_keypair(const void *ikm, size_t n, unsigned char sk[32], unsigned char pk[65]);
/*
 * Sender: a context for pkR from 32 bytes of fresh randomness `ikm_e` (the
 * ephemeral key); `enc` goes to the receiver.
 */
int hpke_setup_sender(const unsigned char pk_r[65], const void *info, size_t in, const unsigned char ikm_e[32],
                      unsigned char enc[65], hpke_t *ctx);
int hpke_setup_receiver(const unsigned char enc[65], const unsigned char sk_r[32], const void *info, size_t in, hpke_t *ctx);
/* `out` gets n + 16 bytes. */
int hpke_seal(hpke_t *ctx, const void *aad, size_t an, const void *pt, size_t n, unsigned char *out);
/* `out` gets n - 16 bytes; 0 when the ciphertext does not authenticate. */
int hpke_open(hpke_t *ctx, const void *aad, size_t an, const void *ct, size_t n, unsigned char *out);
