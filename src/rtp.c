#include <string.h>
#include "rtp.h"
#include "aes.h"

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned get16(const unsigned char *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static unsigned get32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

int rtp_seal_ext(const unsigned char key[32], const rtp_header_t *h, unsigned long nonce, const unsigned char *ext,
                 size_t ext_n, const void *payload, size_t n, sb_t *out)
{
    unsigned char head[16], iv[12] = {0};
    unsigned char *body;
    size_t hn = ext_n ? 16 : 12, words = (ext_n + 3) / 4, sealed = words * 4 + n;
    int ok;

    head[0] = (unsigned char)(ext_n ? 0x90 : 0x80);
    head[1] = (unsigned char)h->type;
    put16(head + 2, h->seq);
    put32(head + 4, h->timestamp);
    put32(head + 8, h->ssrc);
    put16(head + 12, 0xBEDE); /* one-byte extension elements; the preamble stays clear */
    put16(head + 14, (unsigned)words);
    put32(iv, nonce);
    sb_reserve(out, hn + sealed + 16 + 4);
    body = (unsigned char *)out->data + out->len;
    memcpy(body, head, hn);
    /* The elements (zero-padded to whole words) are sealed with the payload. */
    memset(body + hn, 0, words * 4);
    if (ext_n)
        memcpy(body + hn, ext, ext_n);
    memcpy(body + hn + words * 4, payload, n);
    ok = aes_gcm_seal(key, 32, iv, head, hn, body + hn, sealed, body + hn, body + hn + sealed, 16);
    memcpy(body + hn + sealed + 16, iv, 4);
    if (ok) {
        out->len += hn + sealed + 16 + 4;
        out->data[out->len] = 0;
    }
    return ok;
}

int rtp_seal(const unsigned char key[32], const rtp_header_t *h, unsigned long nonce, const void *payload, size_t n,
             sb_t *out)
{
    return rtp_seal_ext(key, h, nonce, NULL, 0, payload, n, out);
}

int rtp_open(const unsigned char key[32], const unsigned char *pkt, size_t n, rtp_header_t *h, sb_t *payload)
{
    unsigned char iv[12] = {0};
    size_t head, ext = 0, at = payload->len;

    if (n < 12 + 16 + 4 || (pkt[0] >> 6) != 2 || (pkt[1] >= 200 && pkt[1] <= 206))
        return 0;
    head = 12 + 4 * (size_t)(pkt[0] & 15);
    if (pkt[0] & 0x10) { /* the extension's profile and length stay clear */
        if (n < head + 4)
            return 0;
        ext = 4 * (size_t)get16(pkt + head + 2);
        head += 4;
    }
    if (n < head + 16 + 4)
        return 0;
    h->type = pkt[1] & 0x7F;
    h->marker = pkt[1] >> 7;
    h->seq = get16(pkt + 2);
    h->timestamp = get32(pkt + 4);
    h->ssrc = get32(pkt + 8);
    memcpy(iv, pkt + n - 4, 4);
    sb_reserve(payload, n);
    if (!aes_gcm_open(key, 32, iv, pkt, head, pkt + head, n - head - 20, payload->data + at, pkt + n - 20, 16))
        return 0;
    if (ext > n - head - 20)
        return 0;
    /* Drop the extension's body, which was encrypted with the media. */
    memmove(payload->data + at, payload->data + at + ext, n - head - 20 - ext);
    payload->len = at + n - head - 20 - ext;
    payload->data[payload->len] = 0;
    return 1;
}

int rtcp_seal(const unsigned char key[32], const unsigned char *pkt, size_t n, unsigned long nonce, sb_t *out)
{
    unsigned char iv[12] = {0}, *body;
    int ok;

    if (n < 8)
        return 0;
    put32(iv, nonce);
    sb_reserve(out, n + 16 + 4);
    body = (unsigned char *)out->data + out->len;
    memcpy(body, pkt, 8);
    ok = aes_gcm_seal(key, 32, iv, pkt, 8, pkt + 8, n - 8, body + 8, body + n, 16);
    memcpy(body + n + 16, iv, 4);
    if (ok) {
        out->len += n + 16 + 4;
        out->data[out->len] = 0;
    }
    return ok;
}

int rtcp_open(const unsigned char key[32], const unsigned char *pkt, size_t n, sb_t *out)
{
    unsigned char iv[12] = {0};

    if (n < 8 + 16 + 4 || (pkt[0] >> 6) != 2 || pkt[1] < 200 || pkt[1] > 206)
        return 0;
    memcpy(iv, pkt + n - 4, 4);
    sb_clear(out);
    sb_reserve(out, n);
    memcpy(out->data, pkt, 8);
    if (!aes_gcm_open(key, 32, iv, pkt, 8, pkt + 8, n - 8 - 20, (unsigned char *)out->data + 8, pkt + n - 20, 16))
        return 0;
    out->len = n - 20;
    out->data[out->len] = 0;
    return 1;
}

unsigned rtcp_key_frame_request(const unsigned char *p, size_t n)
{
    /* Compound packets: PLI (206, format 1) and FIR (206, format 4) name the media they ask about. */
    while (n >= 12) {
        size_t len = 4 * ((size_t)get16(p + 2) + 1);
        if (len > n)
            break;
        if (p[1] == 206 && ((p[0] & 31) == 1 || (p[0] & 31) == 4))
            return (p[0] & 31) == 1 ? get32(p + 8) : len >= 20 ? get32(p + 12) : 0;
        p += len;
        n -= len;
    }
    return 0;
}

void rtcp_pli(unsigned sender_ssrc, unsigned media_ssrc, unsigned char out[12])
{
    out[0] = 0x81; /* version 2, feedback message type 1 */
    out[1] = 206;  /* payload-specific feedback */
    put16(out + 2, 2);
    put32(out + 4, sender_ssrc);
    put32(out + 8, media_ssrc);
}

void rtp_discovery_request(unsigned ssrc, unsigned char out[74])
{
    memset(out, 0, 74);
    put16(out, 1);
    put16(out + 2, 70);
    put32(out + 4, ssrc);
}

int rtp_discovery_response(const unsigned char *in, size_t n, char ip[64], unsigned *port)
{
    size_t k;

    if (n < 74 || get16(in) != 2)
        return 0;
    for (k = 0; k < 63 && in[8 + k]; k++)
        ip[k] = (char)in[8 + k];
    ip[k] = 0;
    *port = get16(in + 72);
    return k > 0;
}
