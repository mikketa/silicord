/* RTP under aead_aes256_gcm_rtpsize, header extensions, RTCP and IP discovery. */
#include "test.h"
#include "aes.h"
#include "rtp.h"
#include "sb.h"
#include "sha2.h"

static const unsigned char k_key[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static void round_trip(void)
{
    static const unsigned char opus[] = {0xFC, 0xFF, 0xFE, 1, 2, 3, 4, 5};
    rtp_header_t h = {513, 960 * 7, 0xDEADBEEF, RTP_OPUS}, got;
    sb_t pkt = {0}, out = {0};
    int ok;

    ok = rtp_seal(k_key, &h, 0x01020304, opus, sizeof opus, &pkt);
    check(ok && pkt.len == 12 + sizeof opus + 16 + 4, "sealed size");
    check((unsigned char)pkt.data[0] == 0x80 && (unsigned char)pkt.data[1] == 0x78 && (unsigned char)pkt.data[2] == 2 &&
              (unsigned char)pkt.data[3] == 1 && (unsigned char)pkt.data[pkt.len - 4] == 1 &&
              (unsigned char)pkt.data[pkt.len - 1] == 4,
          "header and nonce counter");
    ok = rtp_open(k_key, (const unsigned char *)pkt.data, pkt.len, &got, &out);
    check(ok && got.seq == 513 && got.timestamp == 960 * 7 && got.ssrc == 0xDEADBEEF && got.type == RTP_OPUS &&
              out.len == sizeof opus && ct_equal(out.data, opus, sizeof opus),
          "opens");
    pkt.data[1] ^= 1; /* the header is authenticated */
    sb_clear(&out);
    check(!rtp_open(k_key, (const unsigned char *)pkt.data, pkt.len, &got, &out), "tampered header");
    ((unsigned char *)pkt.data)[1] = 200; /* RTCP sender report */
    check(!rtp_open(k_key, (const unsigned char *)pkt.data, pkt.len, &got, &out), "RTCP is not media");
    sb_free(&pkt);
    sb_free(&out);
}

/* A header extension (as Discord sends for audio levels): its body is encrypted with the media and dropped. */
static void extension(void)
{
    static const unsigned char media[] = {9, 8, 7, 6, 5};
    unsigned char pkt[16 + 4 + 5 + 16 + 4], iv[12] = {0}, plain[4 + 5];
    rtp_header_t got;
    sb_t out = {0};

    pkt[0] = 0x90;
    pkt[1] = 0x78;
    pkt[2] = 0;
    pkt[3] = 9;
    for (int i = 4; i < 12; i++)
        pkt[i] = (unsigned char)i;
    pkt[12] = 0xBE;
    pkt[13] = 0xDE;
    pkt[14] = 0;
    pkt[15] = 1; /* one 32-bit word */
    plain[0] = 0x10;
    plain[1] = 0x7F;
    plain[2] = plain[3] = 0;
    for (int i = 0; i < 5; i++)
        plain[4 + i] = media[i];
    iv[3] = 42;
    aes_gcm_seal(k_key, 32, iv, pkt, 16, plain, 9, pkt + 16, pkt + 25, 16);
    pkt[41] = pkt[42] = pkt[43] = 0;
    pkt[44] = 42;
    check(rtp_open(k_key, pkt, sizeof pkt, &got, &out) && out.len == 5 && ct_equal(out.data, media, 5) &&
              got.seq == 9,
          "header extension");
    sb_free(&out);
}

static void discovery(void)
{
    unsigned char req[74], resp[74] = {0};
    char ip[64];
    unsigned port = 0;

    rtp_discovery_request(0x11223344, req);
    check(req[0] == 0 && req[1] == 1 && req[2] == 0 && req[3] == 70 && req[4] == 0x11 && req[7] == 0x44 && !req[8],
          "discovery request");
    resp[1] = 2;
    resp[3] = 70;
    for (int i = 0; "203.0.113.7"[i]; i++)
        resp[8 + i] = (unsigned char)"203.0.113.7"[i];
    resp[72] = 0xC3;
    resp[73] = 0x50;
    check(rtp_discovery_response(resp, sizeof resp, ip, &port) && port == 50000 && ip[0] == '2' && ip[10] == '7' &&
              !ip[11],
          "discovery response");
}

void entry(void)
{
    round_trip();
    extension();
    discovery();
    finish();
}
