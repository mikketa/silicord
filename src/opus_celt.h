#pragma once
#include "opus_rc.h"

/*
 * The CELT layer of Opus (RFC 6716, section 4.3): 48 kHz, 21 bands,
 * frames of 2.5 to 20 ms, in floating point. Its parsing is bit-exact
 * with the reference; the synthesis is within the conformance tolerance.
 */

#define CELT_BANDS 21
#define CELT_OVERLAP 120
#define CELT_BUFFER 2048

/* Working buffers, kept out of the stack (no C runtime means no stack probes past 4 KB). */
typedef struct {
    float freq[2 * 960], x[2 * 960];
    float norm[2 * 800], lowband[176];
    float syn[960 + CELT_OVERLAP];
    float z[2 * 480], f[2 * 480], f2[960];
} celt_scratch_t;

typedef struct {
    int channels;        /* output channels */
    int stream_channels; /* coded in the packet */
    int start, end;      /* coded bands: 17..21 in hybrid mode, up to 13..21 by bandwidth */
    unsigned rng;        /* the range coder's final state, for conformance checks */
    int loss_count;
    int postfilter_period, postfilter_period_old;
    float postfilter_gain, postfilter_gain_old;
    int postfilter_tapset, postfilter_tapset_old;
    float preemph_mem[2];
    float mem[2][CELT_BUFFER + CELT_OVERLAP]; /* past output, then the overlap */
    float old_band_e[2 * CELT_BANDS], old_log_e[2 * CELT_BANDS], old_log_e2[2 * CELT_BANDS];
    float background_log_e[2 * CELT_BANDS];
    celt_scratch_t tmp;
} celt_decoder_t;

void celt_init(celt_decoder_t *st, int channels);
void celt_reset(celt_decoder_t *st);
/*
 * Decodes one frame of `frame_size` samples (120 << LM) into interleaved
 * floats in [-1, 1]. `rc` continues a range decoder (hybrid mode) or is
 * NULL; `data` NULL or len <= 1 conceals a lost frame. Returns the samples, or -1.
 */
int celt_decode(celt_decoder_t *st, const unsigned char *data, int len, float *pcm, int frame_size, opus_rc_t *rc);

/* A mono CELT encoder: 20 ms frames of 960 samples at 48 kHz, long blocks, constant size. */
typedef struct {
    float preemph_mem;
    float in_mem[CELT_OVERLAP];
    float old_band_e[CELT_BANDS];
    int last_coded_bands, frames;
    unsigned rng; /* the range coder's final state, as the decoder will find it */
    float in[960 + CELT_OVERLAP], freq[960], x[960];
    float z[2 * 480], f[2 * 480];
} celt_encoder_t;

void celt_encoder_init(celt_encoder_t *st);
/* Encodes 960 samples in [-1, 1] into exactly `n` bytes (a CELT frame, no TOC). Returns 0 on failure. */
int celt_encode(celt_encoder_t *st, const float *pcm, unsigned char *out, int n);
