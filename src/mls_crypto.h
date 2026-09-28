#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * The labeled primitives of MLS 1.0 (RFC 9420, section 5) for ciphersuite 2,
 * DHKEMP256_AES128GCM_SHA256_P256: labels get the "MLS 1.0 " prefix here.
 */

/* ExpandWithLabel(secret, label, context, len); `secret` of any size (DAVE's ratchets start from 16 bytes). */
int mls_expand_with_label(const unsigned char *secret, size_t sn, const char *label, const void *ctx, size_t cn,
                          unsigned char *out, size_t len);
/* The same with a label of any bytes (the exporter's labels are arbitrary). */
int mls_expand_with_label_n(const unsigned char *secret, size_t sn, const void *label, size_t ln, const void *ctx,
                            size_t cn, unsigned char *out, size_t len);
/* DeriveSecret: ExpandWithLabel(secret, label, "", 32). */
int mls_derive_secret(const unsigned char secret[32], const char *label, unsigned char out[32]);
/* DeriveTreeSecret: the context is the generation as a uint32. */
int mls_derive_tree_secret(const unsigned char *secret, size_t sn, const char *label, unsigned long generation,
                           unsigned char *out, size_t len);
/* RefHash(label, value): SHA-256 over the label and value as vectors; the label is used as given. */
void mls_ref_hash(const char *label, const void *value, size_t n, unsigned char out[32]);

/* SignWithLabel: ECDSA P-256 over SHA-256, the signature DER-encoded into `sig`. */
int mls_sign_with_label(const unsigned char sk[32], const char *label, const void *content, size_t n, sb_t *sig);
int mls_verify_with_label(const unsigned char pub[65], const char *label, const void *content, size_t n,
                          const unsigned char *sig, size_t sn);

/* EncryptWithLabel: HPKE base mode to `pub`, with fresh randomness. */
int mls_encrypt_with_label(const unsigned char pub[65], const char *label, const void *ctx, size_t cn, const void *pt,
                           size_t n, unsigned char kem_output[65], sb_t *ct);
int mls_decrypt_with_label(const unsigned char sk[32], const char *label, const void *ctx, size_t cn,
                           const unsigned char kem_output[65], const unsigned char *ct, size_t n, sb_t *pt);

/* ECDSA signatures between r || s and DER. */
void der_from_rs(const unsigned char rs[64], sb_t *der);
int der_to_rs(const unsigned char *der, size_t n, unsigned char rs[64]);
