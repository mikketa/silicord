/* Opus packet framing (RFC 6716, section 3) and the range decoder's basic arithmetic. */
#include "test.h"
#include "opus.h"
#include "opus_rc.h"

static void framing(void)
{
    static const unsigned char code0[] = {1 << 3, 1, 2, 3};
    static const unsigned char code1[] = {29 << 3 | 1, 1, 2, 3, 4};
    static const unsigned char code2[] = {15 << 3 | 2, 2, 7, 7, 9, 9, 9};
    static const unsigned char code3_cbr[] = {31 << 3 | 4 | 3, 4, 1, 2, 3, 4, 5, 6, 7, 8};
    static const unsigned char code3_vbr_pad[] = {15 << 3 | 3, 0x80 | 0x40 | 2, 2, 3, 1, 1, 1, 2, 2, 0, 0};
    opus_packet_t p;

    check(opus_packet_parse(code0, sizeof code0, &p) && p.count == 1 && p.sizes[0] == 3 && p.mode == OPUS_SILK &&
              p.bandwidth == OPUS_NB && p.frame_samples == 960,
          "code 0: one NB 20 ms SILK frame");
    check(opus_packet_parse(code1, sizeof code1, &p) && p.count == 2 && p.sizes[0] == 2 && p.frames[1] == code1 + 3 &&
              p.mode == OPUS_CELT && p.frame_samples == 240,
          "code 1: two FB 5 ms CELT frames");
    check(opus_packet_parse(code2, sizeof code2, &p) && p.count == 2 && p.sizes[0] == 2 && p.sizes[1] == 3 &&
              p.mode == OPUS_HYBRID && p.bandwidth == OPUS_FB,
          "code 2: two hybrid frames of different sizes");
    check(opus_packet_parse(code3_cbr, sizeof code3_cbr, &p) && p.count == 4 && p.sizes[3] == 2 && p.stereo,
          "code 3 CBR: four stereo CELT frames");
    check(opus_packet_parse(code3_vbr_pad, sizeof code3_vbr_pad, &p) && p.count == 2 && p.sizes[0] == 3 &&
              p.sizes[1] == 2 && p.frames[1][0] == 2,
          "code 3 VBR with padding");

    {
        static const unsigned char odd[] = {29 << 3 | 1, 1, 2, 3};
        static const unsigned char short2[] = {15 << 3 | 2, 252};
        static const unsigned char long2[] = {15 << 3 | 2, 5, 1};
        static const unsigned char zero_m[] = {31 << 3 | 3, 0};
        static const unsigned char too_long[] = {3 << 3 | 3, 3}; /* 3 x 60 ms */
        static const unsigned char cbr_rest[] = {31 << 3 | 3, 2, 1, 2, 3};
        check(!opus_packet_parse(code0, 0, &p), "R1: empty packet");
        check(!opus_packet_parse(odd, sizeof odd, &p), "R3: odd code 1 payload");
        check(!opus_packet_parse(short2, sizeof short2, &p) && !opus_packet_parse(long2, sizeof long2, &p),
              "R4: code 2 lengths");
        check(!opus_packet_parse(zero_m, sizeof zero_m, &p) && !opus_packet_parse(too_long, sizeof too_long, &p),
              "R5: frame count");
        check(!opus_packet_parse(cbr_rest, sizeof cbr_rest, &p), "R6: CBR remainder");
    }
}

static void range_decoder(void)
{
    static const unsigned char zeros[4] = {0};
    static const unsigned char icdf[] = {128, 0}; /* two equiprobable symbols of 8 bits */
    opus_rc_t rc;

    rc_init(&rc, zeros, sizeof zeros);
    check(rc_tell(&rc) == 1 && rc_tell_frac(&rc) == 8, "a new decoder has used one bit");
    /* All-zero input is the lowest coded value: the first symbol every time. */
    check(rc_icdf(&rc, icdf, 8) == 0 && rc_bit_logp(&rc, 1) == 0 && rc_uint(&rc, 1000) == 0, "zeros decode low");
    check(rc_bits(&rc, 4) == 0 && !rc.error, "raw bits of a zero frame");
    check(rc_ilog(0) == 0 && rc_ilog(1) == 1 && rc_ilog(255) == 8 && rc_ilog(256) == 9, "ilog");
}

void entry(void)
{
    framing();
    range_decoder();
    finish();
}
