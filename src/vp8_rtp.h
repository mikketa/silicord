#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * VP8 over RTP (RFC 7741): the payload descriptor, and frames rebuilt
 * from their packets. Packets of one frame share a timestamp; the first
 * starts partition 0 (S set, PID 0) and the last carries the marker bit.
 * Packets may arrive out of order; a frame missing a packet is dropped,
 * and the loss is reported so a key frame can be asked for.
 */

#define VP8_RTP_MAX_PACKETS 256 /* per frame: 300 KB at 1200 bytes a packet */

typedef struct {
    unsigned timestamp;
    int state; /* 0 idle, 1 collecting the frame of `timestamp`, 2 that frame delivered */
    int lost;
    int n;
    struct {
        unsigned short seq;
        unsigned char start, marker;
        size_t at, len;
    } pkts[VP8_RTP_MAX_PACKETS];
    sb_t data;
} vp8_rtp_t;

/* The payload descriptor's length; 0 if malformed. `start` is set for a frame's first packet. */
size_t vp8_rtp_descriptor(const unsigned char *p, size_t n, int *start);
/* Adds a packet. Returns 1 when it completes a frame, now in `frame`; 0 otherwise. */
int vp8_rtp_push(vp8_rtp_t *r, unsigned seq, unsigned timestamp, int marker, const unsigned char *p, size_t n, sb_t *frame);
/* Whether a frame was lost since the last call (and forgets it). */
int vp8_rtp_lost(vp8_rtp_t *r);
void vp8_rtp_free(vp8_rtp_t *r);

/*
 * VP8 frames into RTP payloads of at most `mtu` bytes, each with a
 * descriptor (extended, with a 15-bit picture ID). Calls emit() per packet;
 * the last one should carry the marker bit.
 */
void vp8_rtp_packetize(const unsigned char *frame, size_t n, unsigned picture_id, size_t mtu,
                       void (*emit)(void *ctx, const unsigned char *payload, size_t n, int last), void *ctx);
