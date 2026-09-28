#include "opus.h"

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
