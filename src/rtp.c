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

int rtp_seal(const unsigned char key[32], const rtp_header_t *h, unsigned long nonce, const void *payload, size_t n,
             sb_t *out)
{
    unsigned char head[12], iv[12] = {0};
    unsigned char *body;
    int ok;

    head[0] = 0x80;
    head[1] = (unsigned char)h->type;
    put16(head + 2, h->seq);
    put32(head + 4, h->timestamp);
    put32(head + 8, h->ssrc);
    put32(iv, nonce);
    sb_reserve(out, 12 + n + 16 + 4);
    body = (unsigned char *)out->data + out->len;
    memcpy(body, head, 12);
    ok = aes_gcm_seal(key, 32, iv, head, 12, payload, n, body + 12, body + 12 + n, 16);
    memcpy(body + 12 + n + 16, iv, 4);
    if (ok) {
        out->len += 12 + n + 16 + 4;
        out->data[out->len] = 0;
    }
    return ok;
}

int rtp_open(const unsigned char key[32], const unsigned char *pkt, size_t n, rtp_header_t *h, sb_t *payload)
{
    unsigned char iv[12] = {0};
    size_t head, ext = 0, at = payload->len;

    if (n < 12 + 16 + 4 || (pkt[0] >> 6) != 2 || (pkt[1] >= 200 && pkt[1] <= 204))
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
