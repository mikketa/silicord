#pragma once
#include <stddef.h>
#include "opus_celt.h"
#include "opus_silk.h"

/* Opus (RFC 6716): packet framing, and the decoder built on SILK and CELT. */

/* Modes start at 1: a decoder's previous mode is 0 before its first packet. */
enum { OPUS_SILK = 1, OPUS_HYBRID, OPUS_CELT };
enum { OPUS_NB, OPUS_MB, OPUS_WB, OPUS_SWB, OPUS_FB };

typedef struct {
    int config, stereo;
    int mode, bandwidth;
    int frame_samples; /* per frame, at 48 kHz */
    int count;
    const unsigned char *frames[48];
    unsigned short sizes[48]; /* 0 for a missing frame (DTX or loss) */
} opus_packet_t;

/* Splits a packet into frames, enforcing the rules of section 3.4; 0 when malformed. */
int opus_packet_parse(const unsigned char *data, size_t n, opus_packet_t *p);

/* A decoder for one stream at 48 kHz. */
typedef struct {
    int channels;
    celt_decoder_t celt;
    silk_decoder_t silk;
    int mode, prev_mode, prev_redundancy, bandwidth, frame_size, stream_channels;
    unsigned final_range; /* the last frame's range coder state, for conformance checks */
    short pcm_silk[2 * 2880]; /* up to 60 ms of SILK */
    float transition[2 * 240], redundant[2 * 240];
} opus_decoder_t;

void opus_decoder_init(opus_decoder_t *d, int channels);
/* Decodes a packet (NULL or empty for a lost one of `lost_samples`) into interleaved floats;
   returns the samples per channel, or -1 for a malformed packet. `pcm` holds 5760 per channel. */
int opus_decode(opus_decoder_t *d, const unsigned char *data, size_t n, float *pcm, int lost_samples);

/* A mono voice encoder: CELT fullband, 20 ms frames, 64 kbit/s constant bitrate. */
typedef struct {
    celt_encoder_t celt;
} opus_encoder_t;

void opus_encoder_init(opus_encoder_t *e);
/* Encodes 960 mono samples in [-1, 1] into an Opus packet; returns its size, or 0. `out` holds 1276 bytes. */
int opus_encode(opus_encoder_t *e, const float *pcm, unsigned char *out);
