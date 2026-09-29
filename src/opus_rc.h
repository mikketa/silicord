#pragma once
#include <stddef.h>

/*
 * Opus's range decoder (RFC 6716, section 4.1): symbols coded from the
 * front of a frame, raw bits from its back. Everything is bit-exact
 * integer arithmetic; reads past the end yield zeros.
 */

typedef struct {
    const unsigned char *buf;
    unsigned storage;     /* frame size in bytes */
    unsigned offs;        /* next byte for the range coder */
    unsigned end_offs;    /* bytes taken from the end as raw bits */
    unsigned end_window;  /* raw bits read but not used */
    int nend_bits;
    int nbits_total;
    unsigned rng, val;
    unsigned ext;         /* rng / ft from the last rc_decode() */
    int rem;              /* the last byte read, whose low bit is still pending */
} opus_rc_t;

void rc_init(opus_rc_t *rc, const unsigned char *buf, unsigned n);
/* The first step of decoding a symbol: fs, with fl[k] <= fs < fh[k]. */
unsigned rc_decode(opus_rc_t *rc, unsigned ft);
unsigned rc_decode_bin(opus_rc_t *rc, unsigned bits);
/* The second step: consume symbol (fl, fh) of ft. */
void rc_update(opus_rc_t *rc, unsigned fl, unsigned fh, unsigned ft);
/* A bit whose "1" has probability 2^-logp. */
int rc_bit_logp(opus_rc_t *rc, unsigned logp);
/* A symbol from an inverse CDF table ending in 0, over 2^ftb. */
int rc_icdf(opus_rc_t *rc, const unsigned char *icdf, unsigned ftb);
/* One of ft equiprobable values (ft up to 2^32 - 1). */
unsigned rc_uint(opus_rc_t *rc, unsigned ft);
/* Raw bits from the end of the frame (up to 25 at a time). */
unsigned rc_bits(opus_rc_t *rc, unsigned n);
/* Bits used so far, whole and in 1/8 bits. */
int rc_tell(const opus_rc_t *rc);
unsigned rc_tell_frac(const opus_rc_t *rc);
/* Number of bits needed for x (0 for 0). */
int rc_ilog(unsigned x);

/* The range encoder (section 5.1), writing into a fixed-size frame. */
typedef struct {
    unsigned char *buf;
    unsigned storage, offs, end_offs, end_window;
    int nend_bits, nbits_total;
    unsigned rng, val, ext;
    int rem, error;
} opus_rce_t;

void rce_init(opus_rce_t *e, unsigned char *buf, unsigned n);
void rce_encode(opus_rce_t *e, unsigned fl, unsigned fh, unsigned ft);
void rce_encode_bin(opus_rce_t *e, unsigned fl, unsigned fh, unsigned bits);
void rce_bit_logp(opus_rce_t *e, int val, unsigned logp);
void rce_icdf(opus_rce_t *e, int s, const unsigned char *icdf, unsigned ftb);
void rce_uint(opus_rce_t *e, unsigned fl, unsigned ft);
void rce_bits(opus_rce_t *e, unsigned fl, unsigned n);
int rce_tell(const opus_rce_t *e);
unsigned rce_tell_frac(const opus_rce_t *e);
/* Flushes the frame; returns 0 when the symbols did not fit. */
int rce_done(opus_rce_t *e);
