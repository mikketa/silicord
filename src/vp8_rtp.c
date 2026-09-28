#include <string.h>
#include "vp8_rtp.h"

size_t vp8_rtp_descriptor(const unsigned char *p, size_t n, int *start)
{
    size_t k = 1;

    if (n < 1)
        return 0;
    *start = (p[0] & 0x10) && (p[0] & 0x07) == 0;
    if (p[0] & 0x80) { /* extensions: picture ID, TL0PICIDX, TID/KEYIDX */
        unsigned x;
        if (n < 2)
            return 0;
        x = p[1];
        k = 2;
        if (x & 0x80) {
            if (n < k + 1)
                return 0;
            k += p[k] & 0x80 ? 2 : 1;
        }
        if (x & 0x40)
            k++;
        if (x & 0x30)
            k++;
    }
    return k < n ? k : 0;
}

/* The frame, if every packet from its start to its marker is here. */
static int complete(vp8_rtp_t *r, sb_t *frame)
{
    int first = -1, last = -1;
    unsigned span;

    for (int i = 0; i < r->n; i++) {
        if (r->pkts[i].start)
            first = i;
        if (r->pkts[i].marker)
            last = i;
    }
    if (first < 0 || last < 0)
        return 0;
    span = (unsigned short)(r->pkts[last].seq - r->pkts[first].seq);
    if (span >= (unsigned)r->n)
        return 0;
    sb_clear(frame);
    for (unsigned k = 0; k <= span; k++) {
        unsigned short want = (unsigned short)(r->pkts[first].seq + k);
        int i = 0;
        while (i < r->n && r->pkts[i].seq != want)
            i++;
        if (i == r->n)
            return 0;
        sb_addn(frame, r->data.data + r->pkts[i].at, r->pkts[i].len);
    }
    return 1;
}

int vp8_rtp_push(vp8_rtp_t *r, unsigned seq, unsigned timestamp, int marker, const unsigned char *p, size_t n, sb_t *frame)
{
    int start;
    size_t k = vp8_rtp_descriptor(p, n, &start);

    if (!k)
        return 0;
    if (r->state && timestamp != r->timestamp) {
        if ((int)(timestamp - r->timestamp) < 0)
            return 0; /* a late packet of an older frame */
        if (r->state == 1)
            r->lost = 1; /* the previous frame never completed */
        r->state = 0;
    }
    if (r->state == 2)
        return 0; /* this frame was already delivered */
    if (!r->state) {
        r->state = 1;
        r->timestamp = timestamp;
        r->n = 0;
        sb_clear(&r->data);
    }
    for (int i = 0; i < r->n; i++)
        if (r->pkts[i].seq == (unsigned short)seq)
            return 0;
    if (r->n == VP8_RTP_MAX_PACKETS) {
        r->lost = 1;
        r->state = 2;
        return 0;
    }
    r->pkts[r->n].seq = (unsigned short)seq;
    r->pkts[r->n].start = (unsigned char)start;
    r->pkts[r->n].marker = (unsigned char)(marker != 0);
    r->pkts[r->n].at = r->data.len;
    r->pkts[r->n].len = n - k;
    sb_addn(&r->data, (const char *)p + k, n - k);
    r->n++;
    if (!complete(r, frame))
        return 0;
    r->state = 2;
    r->n = 0;
    return 1;
}

int vp8_rtp_lost(vp8_rtp_t *r)
{
    int lost = r->lost;

    r->lost = 0;
    return lost;
}

void vp8_rtp_free(vp8_rtp_t *r)
{
    sb_free(&r->data);
    memset(r, 0, sizeof *r);
}

void vp8_rtp_packetize(const unsigned char *frame, size_t n, unsigned picture_id, size_t mtu,
                       void (*emit)(void *ctx, const unsigned char *payload, size_t n, int last), void *ctx)
{
    unsigned char pkt[1500];
    size_t room = (mtu > sizeof pkt ? sizeof pkt : mtu) - 4, at = 0;

    do {
        size_t len = n - at < room ? n - at : room;
        pkt[0] = (unsigned char)(at == 0 ? 0x90 : 0x80); /* X, and S on the first packet */
        pkt[1] = 0x80;                                  /* I: a picture ID follows */
        pkt[2] = (unsigned char)(0x80 | (picture_id >> 8 & 0x7F));
        pkt[3] = (unsigned char)picture_id;
        memcpy(pkt + 4, frame + at, len);
        at += len;
        emit(ctx, pkt, len + 4, at == n);
    } while (at < n);
}
