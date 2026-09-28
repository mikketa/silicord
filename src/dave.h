#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * DAVE media frames (Discord's audio/video E2EE, protocol 1): per-sender key
 * ratchets and the frame format, independent of the MLS group that yields
 * each sender's base secret.
 *
 * Frame: interleaved media || 8-byte AES-128-GCM tag || ULEB128 nonce ||
 * ULEB128 (offset, length) unencrypted ranges || supplemental size || FA FA.
 */

#define DAVE_KEEP 8 /* past generations whose keys stay available */

typedef struct {
    unsigned char next_secret[32];
    size_t secret_len;
    unsigned long next_gen;
    unsigned long gens[DAVE_KEEP];
    unsigned char keys[DAVE_KEEP][16];
    int nkeys;
} dave_ratchet_t;

/* A sender's ratchet from MLS-Exporter("Discord Secure Frames v0", little-endian user id, 16). */
void dave_ratchet_init(dave_ratchet_t *r, const unsigned char base_secret[16]);
/* The key of a generation, ratcheting forward as needed; 0 for an erased generation. */
int dave_ratchet_key(dave_ratchet_t *r, unsigned long generation, unsigned char key[16]);
void dave_ratchet_wipe(dave_ratchet_t *r);

typedef struct {
    size_t offset, length;
} dave_range_t;

/*
 * Encrypts a frame: everything outside the (ascending, disjoint) unencrypted
 * ranges is encrypted; the ranges are authenticated. Opus frames have none.
 * The generation is the nonce's most significant byte.
 */
int dave_encrypt(const unsigned char key[16], unsigned long nonce, const unsigned char *frame, size_t n,
                 const dave_range_t *ranges, int nranges, sb_t *out);
/* The frame's nonce (and so its generation) without decrypting; 0 if it is no protocol frame. */
int dave_frame_nonce(const unsigned char *in, size_t n, unsigned long *nonce);
/* Decrypts a protocol frame with the sender's ratchet. */
int dave_decrypt(dave_ratchet_t *r, const unsigned char *in, size_t n, sb_t *out);
/* The SFU's synthesized silence, passed through untouched. */
int dave_is_silence(const unsigned char *in, size_t n);

/* Displayable code: `len` digits in groups of `group` (epoch authenticator: 30 digits of 5). */
int dave_displayable_code(const unsigned char *data, size_t n, int len, int group, char *out, size_t out_size);
