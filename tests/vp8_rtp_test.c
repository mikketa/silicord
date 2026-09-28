/* VP8 over RTP (RFC 7741): descriptors, packetizing, and frames rebuilt from shuffled or missing packets. */
#include <string.h>
#include "test.h"
#include "vp8_rtp.h"

typedef struct {
    unsigned char data[8][1300];
    size_t len[8];
    int last[8], n;
} packets_t;

static void collect(void *ctx, const unsigned char *payload, size_t n, int last)
{
    packets_t *p = ctx;

    if (p->n < 8) {
        memcpy(p->data[p->n], payload, n);
        p->len[p->n] = n;
        p->last[p->n] = last;
        p->n++;
    }
}

static int same(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static void descriptors(void)
{
    static const unsigned char plain[] = {0x10, 0xAA}, ext[] = {0x90, 0xE0, 0x81, 0x23, 0x05, 0x40, 0xAA},
                               middle[] = {0x80, 0x80, 0x12, 0xAA}, part1[] = {0x11, 0xAA};
    int start;

    check(vp8_rtp_descriptor(plain, sizeof plain, &start) == 1 && start, "a bare descriptor starting a frame");
    check(vp8_rtp_descriptor(ext, sizeof ext, &start) == 6 && start, "picture ID (15 bits), TL0PICIDX and TID");
    check(vp8_rtp_descriptor(middle, sizeof middle, &start) == 3 && !start, "a 7-bit picture ID, not a start");
    check(vp8_rtp_descriptor(part1, sizeof part1, &start) == 1 && !start, "the start of partition 1 is not a frame start");
    check(vp8_rtp_descriptor(ext, 3, &start) == 0, "a truncated descriptor");
}

static void round_trip(void)
{
    static packets_t p;
    static vp8_rtp_t r;
    static unsigned char frame[3000];
    static const int order[3] = {2, 0, 1};
    sb_t out = {0};
    int got = 0;

    for (int i = 0; i < (int)sizeof frame; i++)
        frame[i] = (unsigned char)(i * 7);
    vp8_rtp_packetize(frame, sizeof frame, 300, 1200, collect, &p);
    check(p.n == 3 && p.last[2] && !p.last[0] && p.len[0] == 1200, "3000 bytes make three packets of at most 1200");
    /* Out of order, with a duplicate: one frame, identical. */
    for (int k = 0; k < 3; k++)
        got += vp8_rtp_push(&r, 1000u + (unsigned)order[k], 9000, p.last[order[k]], p.data[order[k]], p.len[order[k]], &out);
    got += vp8_rtp_push(&r, 1001, 9000, 0, p.data[1], p.len[1], &out);
    check(got == 1 && out.len == sizeof frame && same(out.data, frame, sizeof frame), "the frame comes back once");
    check(!vp8_rtp_lost(&r), "nothing lost");

    /* The next frame misses its middle packet: nothing, and a loss once the one after begins. */
    got = vp8_rtp_push(&r, 1003, 12000, 0, p.data[0], p.len[0], &out);
    got += vp8_rtp_push(&r, 1005, 12000, 1, p.data[2], p.len[2], &out);
    check(!got && !vp8_rtp_lost(&r), "an incomplete frame waits");
    got = vp8_rtp_push(&r, 1006, 15000, 0, p.data[0], p.len[0], &out);
    check(!got && vp8_rtp_lost(&r), "then counts as lost");
    check(!vp8_rtp_push(&r, 1004, 12000, 0, p.data[1], p.len[1], &out), "its late packet is ignored");

    /* Sequence numbers wrapping inside a frame. */
    got = 0;
    for (int k = 0; k < 3; k++)
        got += vp8_rtp_push(&r, (65535u + (unsigned)k) & 0xFFFF, 18000, p.last[k], p.data[k], p.len[k], &out);
    check(got == 1 && out.len == sizeof frame, "sequence numbers wrap");
    vp8_rtp_free(&r);
    sb_free(&out);
}

void entry(void)
{
    descriptors();
    round_trip();
    finish();
}
