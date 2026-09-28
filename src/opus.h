#pragma once
#include <stddef.h>

/* Opus (RFC 6716): packet framing, and the decoder built on SILK and CELT. */

enum { OPUS_SILK, OPUS_HYBRID, OPUS_CELT };
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
