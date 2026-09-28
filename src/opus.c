#include <string.h>
#include "opus.h"
#include "opus_math.h"

#define MAX_FRAME 1275

/* One or two bytes of frame length; returns the bytes used, 0 when there are not enough. */
static size_t frame_length(const unsigned char *p, size_t n, unsigned *len)
{
    if (n < 1)
        return 0;
    if (p[0] < 252) {
        *len = p[0];
        return 1;
    }
    if (n < 2)
        return 0;
    *len = 4u * p[1] + p[0];
    return 2;
}

int opus_packet_parse(const unsigned char *data, size_t n, opus_packet_t *p)
{
    static const int celt[4] = {120, 240, 480, 960}, silk[4] = {480, 960, 1920, 2880};
    const unsigned char *at;
    size_t left, used;
    unsigned len;
    int code;

    if (n < 1) /* R1 */
        return 0;
    p->config = data[0] >> 3;
    p->stereo = data[0] >> 2 & 1;
    code = data[0] & 3;
    if (p->config < 12) {
        p->mode = OPUS_SILK;
        p->bandwidth = p->config / 4;
        p->frame_samples = silk[p->config & 3];
    } else if (p->config < 16) {
        p->mode = OPUS_HYBRID;
        p->bandwidth = p->config < 14 ? OPUS_SWB : OPUS_FB;
        p->frame_samples = p->config & 1 ? 960 : 480;
    } else {
        static const int bw[4] = {OPUS_NB, OPUS_WB, OPUS_SWB, OPUS_FB};
        p->mode = OPUS_CELT;
        p->bandwidth = bw[(p->config - 16) / 4];
        p->frame_samples = celt[p->config & 3];
    }
    at = data + 1;
    left = n - 1;
    if (code == 0) {
        p->count = 1;
        if (left > MAX_FRAME) /* R2 */
            return 0;
        p->frames[0] = at;
        p->sizes[0] = (unsigned short)left;
        return 1;
    }
    if (code == 1) {
        if (left & 1 || left / 2 > MAX_FRAME) /* R3, R2 */
            return 0;
        p->count = 2;
        p->frames[0] = at;
        p->frames[1] = at + left / 2;
        p->sizes[0] = p->sizes[1] = (unsigned short)(left / 2);
        return 1;
    }
    if (code == 2) {
        used = frame_length(at, left, &len);
        if (!used || len > left - used || left - used - len > MAX_FRAME) /* R4, R2 */
            return 0;
        p->count = 2;
        p->frames[0] = at + used;
        p->sizes[0] = (unsigned short)len;
        p->frames[1] = at + used + len;
        p->sizes[1] = (unsigned short)(left - used - len);
        return 1;
    }
    {
        unsigned char fc;
        size_t padding = 0, total;
        int vbr, count;
        if (left < 1) /* R6, R7 */
            return 0;
        fc = *at++;
        left--;
        vbr = fc >> 7;
        count = fc & 63;
        if (!count || count * p->frame_samples > 5760) /* R5: at most 120 ms */
            return 0;
        if (fc & 0x40) {
            unsigned b;
            do {
                if (left < 1)
                    return 0;
                b = *at++;
                left--;
                padding += b == 255 ? 254 : b;
            } while (b == 255);
        }
        if (padding > left)
            return 0;
        left -= padding;
        p->count = count;
        if (vbr) {
            size_t sum = 0;
            for (int i = 0; i < count - 1; i++) {
                used = frame_length(at, left, &len);
                if (!used)
                    return 0;
                at += used;
                left -= used;
                p->sizes[i] = (unsigned short)len;
                sum += len;
            }
            if (sum > left) /* R7 */
                return 0;
            p->sizes[count - 1] = (unsigned short)(left - sum);
        } else {
            if (left % (size_t)count) /* R6 */
                return 0;
            for (int i = 0; i < count; i++)
                p->sizes[i] = (unsigned short)(left / (size_t)count);
        }
        total = 0;
        for (int i = 0; i < count; i++) {
            if (p->sizes[i] > MAX_FRAME) /* R2 */
                return 0;
            p->frames[i] = at + total;
            total += p->sizes[i];
        }
        return 1;
    }
}

void opus_decoder_init(opus_decoder_t *d, int channels)
{
    memset(d, 0, sizeof *d);
    d->channels = channels;
    d->frame_size = 960;
    celt_init(&d->celt, channels);
    silk_init(&d->silk);
}

/* The last CELT band for each bandwidth. */
static int celt_end_band(int bandwidth)
{
    static const int end[5] = {13, 17, 17, 19, 21};

    return end[bandwidth];
}

/* A 2.5 ms crossfade with the CELT window squared: from in1 to in2. */
static void smooth_fade(const float *in1, const float *in2, float *out, int channels)
{
    static float w2[120];
    static int ready;

    if (!ready) {
        for (int i = 0; i < 120; i++) {
            double s = om_sin(0.5 * OM_PI * (i + 0.5) / 120), w = om_sin(0.5 * OM_PI * s * s);
            w2[i] = (float)(w * w);
        }
        ready = 1;
    }
    for (int c = 0; c < channels; c++)
        for (int i = 0; i < 120; i++)
            out[i * channels + c] = w2[i] * in2[i * channels + c] + (1.f - w2[i]) * in1[i * channels + c];
}

/* Section 4.5: SILK, hybrid and CELT frames, and the redundant 5 ms CELT frames that smooth mode switches. */
static int decode_frame(opus_decoder_t *d, const unsigned char *data, int len, float *pcm, int frame_size)
{
    const int F20 = 960, F5 = 240, F2_5 = 120, cc = d->channels;
    opus_rc_t dec;
    int audiosize, mode, transition = 0, redundancy = 0, redundancy_bytes = 0, celt_to_silk = 0, celt_ret = 0;
    unsigned redundant_rng = 0;

    memset(&dec, 0, sizeof dec);
    if (len <= 1) {
        data = NULL;
        if (frame_size > d->frame_size)
            frame_size = d->frame_size;
    }
    if (data) {
        audiosize = d->frame_size;
        mode = d->mode;
        rc_init(&dec, data, (unsigned)len);
    } else {
        audiosize = frame_size;
        if (d->prev_mode == 0) {
            memset(pcm, 0, sizeof *pcm * (size_t)(audiosize * cc));
            return audiosize;
        }
        mode = d->prev_mode;
    }
    /* Concealment beyond 20 ms of CELT or hybrid, 20 ms at a time. */
    if (!data && frame_size > F20 && mode != OPUS_SILK) {
        int done = 0;
        do {
            if (decode_frame(d, NULL, 0, pcm + done * cc, F20) != F20)
                return -1;
            done += F20;
        } while (done < frame_size);
        return frame_size;
    }
    if (data && d->prev_mode > 0 &&
        ((mode == OPUS_CELT && d->prev_mode != OPUS_CELT && !d->prev_redundancy) ||
         (mode != OPUS_CELT && d->prev_mode == OPUS_CELT))) {
        transition = 1;
        if (mode == OPUS_CELT)
            decode_frame(d, NULL, 0, d->transition, audiosize < F5 ? audiosize : F5);
    }
    if (audiosize > frame_size)
        return -1;
    frame_size = audiosize;

    if (mode != OPUS_CELT) {
        int lost = data ? SILK_DECODE_NORMAL : SILK_PACKET_LOST, decoded = 0, internal_hz = 16000;
        int payload_ms = 1000 * audiosize / 48000;
        short *p = d->pcm_silk;
        if (d->prev_mode == OPUS_CELT)
            silk_init(&d->silk);
        if (mode == OPUS_SILK)
            internal_hz = d->bandwidth == OPUS_NB ? 8000 : d->bandwidth == OPUS_MB ? 12000 : 16000;
        do {
            int got = silk_decode(&d->silk, &dec, cc, d->stream_channels, internal_hz, payload_ms < 10 ? 10 : payload_ms,
                                  lost, decoded == 0, p);
            if (got < 0) {
                if (!lost)
                    return -1;
                got = frame_size;
                memset(p, 0, sizeof *p * (size_t)(frame_size * cc));
            }
            p += got * cc;
            decoded += got;
        } while (decoded < frame_size);
    }

    if (mode != OPUS_CELT && data && rc_tell(&dec) + 17 + 20 * (d->mode == OPUS_HYBRID) <= 8 * len) {
        redundancy = mode == OPUS_HYBRID ? rc_bit_logp(&dec, 12) : 1;
        if (redundancy) {
            celt_to_silk = rc_bit_logp(&dec, 1);
            redundancy_bytes = mode == OPUS_HYBRID ? (int)rc_uint(&dec, 256) + 2 : len - ((rc_tell(&dec) + 7) >> 3);
            len -= redundancy_bytes;
            if (len * 8 < rc_tell(&dec)) {
                len = 0;
                redundancy_bytes = 0;
                redundancy = 0;
            }
            dec.storage -= (unsigned)redundancy_bytes;
        }
    }
    d->celt.end = celt_end_band(d->bandwidth);
    d->celt.stream_channels = d->stream_channels;
    if (redundancy)
        transition = 0;
    if (transition && mode != OPUS_CELT)
        decode_frame(d, NULL, 0, d->transition, audiosize < F5 ? audiosize : F5);

    if (redundancy && celt_to_silk) {
        d->celt.start = 0;
        celt_decode(&d->celt, data + len, redundancy_bytes, d->redundant, F5, NULL);
        redundant_rng = d->celt.rng;
    }
    d->celt.start = mode != OPUS_CELT ? 17 : 0;

    if (mode != OPUS_SILK) {
        if (mode != d->prev_mode && d->prev_mode > 0 && !d->prev_redundancy)
            celt_reset(&d->celt);
        celt_ret = celt_decode(&d->celt, data, len, pcm, frame_size < F20 ? frame_size : F20, data ? &dec : NULL);
    } else {
        static const unsigned char silence[2] = {0xFF, 0xFF};
        memset(pcm, 0, sizeof *pcm * (size_t)(frame_size * cc));
        /* Hybrid to SILK: the CELT MDCT fades out on a silence frame. */
        if (d->prev_mode == OPUS_HYBRID && !(redundancy && celt_to_silk && d->prev_redundancy)) {
            d->celt.start = 0;
            celt_decode(&d->celt, silence, 2, pcm, F2_5, NULL);
        }
    }
    if (mode != OPUS_CELT)
        for (int i = 0; i < frame_size * cc; i++)
            pcm[i] += (1.f / 32768.f) * (float)d->pcm_silk[i];

    if (redundancy && !celt_to_silk) {
        celt_reset(&d->celt);
        d->celt.start = 0;
        celt_decode(&d->celt, data + len, redundancy_bytes, d->redundant, F5, NULL);
        redundant_rng = d->celt.rng;
        smooth_fade(pcm + cc * (frame_size - F2_5), d->redundant + cc * F2_5, pcm + cc * (frame_size - F2_5), cc);
    }
    if (redundancy && celt_to_silk) {
        for (int i = 0; i < F2_5 * cc; i++)
            pcm[i] = d->redundant[i];
        smooth_fade(d->redundant + cc * F2_5, pcm + cc * F2_5, pcm + cc * F2_5, cc);
    }
    if (transition) {
        if (audiosize >= F5) {
            for (int i = 0; i < cc * F2_5; i++)
                pcm[i] = d->transition[i];
            smooth_fade(d->transition + cc * F2_5, pcm + cc * F2_5, pcm + cc * F2_5, cc);
        } else {
            smooth_fade(d->transition, pcm, pcm, cc);
        }
    }
    d->final_range = len <= 1 ? 0 : dec.rng ^ redundant_rng;
    d->prev_mode = mode;
    d->prev_redundancy = redundancy && !celt_to_silk;
    return celt_ret < 0 ? -1 : audiosize;
}

int opus_decode(opus_decoder_t *d, const unsigned char *data, size_t n, float *pcm, int lost_samples)
{
    opus_packet_t p;
    int total = 0;

    if (!data || !n)
        return decode_frame(d, NULL, 0, pcm, lost_samples);
    if (!opus_packet_parse(data, n, &p))
        return -1;
    d->mode = p.mode;
    d->bandwidth = p.bandwidth;
    d->frame_size = p.frame_samples;
    d->stream_channels = p.stereo ? 2 : 1;
    for (int k = 0; k < p.count; k++) {
        int got = decode_frame(d, p.frames[k], p.sizes[k], pcm + total * d->channels, 5760 - total);
        if (got < 0)
            return -1;
        total += got;
    }
    return total;
}
