#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * Discord voice over UDP: RTP packets under the aead_aes256_gcm_rtpsize
 * transport encryption (the header, and a header extension's first 4 bytes,
 * are authenticated but clear; a 4-byte counter after the tag makes the
 * nonce), and the IP discovery exchange.
 */

#define RTP_OPUS 0x78 /* Discord's Opus payload type */

typedef struct {
    unsigned seq, timestamp, ssrc, type;
} rtp_header_t;

/* An Opus RTP packet: the 12-byte header, the sealed payload, the tag, the nonce counter. */
int rtp_seal(const unsigned char key[32], const rtp_header_t *h, unsigned long nonce, const void *payload, size_t n,
             sb_t *out);
/* Opens a packet; `payload` gets the media after any header extension. 0 for RTCP or a bad tag. */
int rtp_open(const unsigned char key[32], const unsigned char *pkt, size_t n, rtp_header_t *h, sb_t *payload);

/* The 74-byte IP discovery request for our SSRC. */
void rtp_discovery_request(unsigned ssrc, unsigned char out[74]);
/* Our address as the voice server sees it. */
int rtp_discovery_response(const unsigned char *in, size_t n, char ip[64], unsigned *port);
