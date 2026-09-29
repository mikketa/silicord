#pragma once
#include <stddef.h>
#include "sb.h"

/* Thin wrappers over Windows CNG (bcrypt.dll). */

typedef struct rsa_key rsa_key_t;

void sha256(const void *data, size_t n, unsigned char out[32]);

/* Fresh RSA-2048 key pair, kept in memory only. */
rsa_key_t *rsa_generate(void);
void rsa_free(rsa_key_t *key);
/* Public key as DER-encoded SubjectPublicKeyInfo (what browsers call "spki"). */
int rsa_public_spki(rsa_key_t *key, sb_t *der);
int rsa_decrypt_oaep_sha256(rsa_key_t *key, const void *in, size_t n, sb_t *out);
