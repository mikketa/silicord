#pragma once
#include <stddef.h>

/*
 * NIST P-256 in portable C, for MLS ciphersuite 2 (DHKEM P-256, ECDSA P-256):
 * constant-time Montgomery arithmetic, complete addition formulas (Renes,
 * Costello, Batina 2016) and a ladder over every bit of secret scalars.
 * Scalars are 32 big-endian bytes; points are SEC1 uncompressed (65 bytes, 0x04 || X || Y).
 */

/* 1 when 0 < sk < n. */
int p256_scalar_ok(const unsigned char sk[32]);
/* The public point of a secret scalar. Returns 0 if the scalar is not valid. */
int p256_public(const unsigned char sk[32], unsigned char pub[65]);
/* ECDH: the X coordinate of sk * pub. Returns 0 on an invalid point or scalar. */
int p256_ecdh(const unsigned char sk[32], const unsigned char pub[65], unsigned char shared[32]);
/* ECDSA over a SHA-256 digest, with RFC 6979's deterministic nonce; sig is r || s. */
int p256_sign(const unsigned char sk[32], const unsigned char hash[32], unsigned char sig[64]);
int p256_verify(const unsigned char pub[65], const unsigned char hash[32], const unsigned char sig[64]);
