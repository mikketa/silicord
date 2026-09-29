/*
 * The VP8 encoder against decoders:
 *
 *   vp8_roundtrip source.ivf kbps ours.ivf ours.yuv
 *   vp8_roundtrip --speed width height frames
 *
 * Re-encodes a test vector's pictures at `kbps`, decodes every frame back
 * with our decoder and requires it to match the encoder's reconstruction
 * exactly, and the pictures to stay above MIN_PSNR. Writes the stream and
 * our decoding (I420) so another decoder can be compared with it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vp8.h"
#include "vp8_enc.h"

#define MIN_PSNR 20.0 /* a sanity floor: vector 014 is noise the bitrate cannot follow */

static unsigned char *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)*n + 1);
    if (buf && fread(buf, 1, (size_t)*n, f) != (size_t)*n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static double psnr(const vp8_image_t *a, const vp8_image_t *b)
{
    double se = 0;

    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            int d = a->y[y * a->y_stride + x] - b->y[y * b->y_stride + x];
            se += d * d;
        }
    se /= (double)a->w * a->h;
    return se > 0 ? 10 * log10(255.0 * 255.0 / se) : 99;
}

static int same(const vp8_image_t *a, const vp8_image_t *b)
{
    for (int y = 0; y < a->h; y++)
        if (memcmp(a->y + y * a->y_stride, b->y + y * b->y_stride, (size_t)a->w))
            return 0;
    for (int y = 0; y < (a->h + 1) / 2; y++)
        if (memcmp(a->u + y * a->uv_stride, b->u + y * b->uv_stride, (size_t)(a->w + 1) / 2) ||
            memcmp(a->v + y * a->uv_stride, b->v + y * b->uv_stride, (size_t)(a->w + 1) / 2))
            return 0;
    return 1;
}

static void put_le(unsigned char *p, unsigned v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void write_yuv(FILE *f, const vp8_image_t *img)
{
    for (int y = 0; y < img->h; y++)
        fwrite(img->y + y * img->y_stride, 1, (size_t)img->w, f);
    for (int y = 0; y < (img->h + 1) / 2; y++)
        fwrite(img->u + y * img->uv_stride, 1, (size_t)(img->w + 1) / 2, f);
    for (int y = 0; y < (img->h + 1) / 2; y++)
        fwrite(img->v + y * img->uv_stride, 1, (size_t)(img->w + 1) / 2, f);
}

/* How fast the encoder runs on a moving synthetic picture (a camera-sized one, say). */
static int speed(int w, int h, int frames)
{
    vp8_encoder_t *enc = vp8_encoder_new(w, h, 800, 30);
    unsigned char *y = malloc((size_t)w * h), *u = malloc((size_t)w * h / 4 + w), *v = malloc((size_t)w * h / 4 + w);
    vp8_image_t img = {w, h, y, u, v, w, (w + 1) / 2};
    sb_t out = {0};
    size_t bytes = 0;
    clock_t start = clock();

    for (int f = 0; f < frames; f++) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                y[j * w + i] = (unsigned char)(((i + 3 * f) ^ (j + f)) & 0xFF) / 2 + (unsigned char)((i * j) >> 10);
        memset(u, 128 + f % 16, (size_t)((w + 1) / 2) * ((h + 1) / 2));
        memset(v, 120, (size_t)((w + 1) / 2) * ((h + 1) / 2));
        vp8_encode(enc, &img, 0, &out);
        bytes += out.len;
    }
    printf("%dx%d: %.1f ms a frame, %zu bytes a frame\n", w, h,
           (double)(clock() - start) * 1000 / CLOCKS_PER_SEC / frames, bytes / (size_t)frames);
    vp8_encoder_free(enc);
    free(y);
    free(u);
    free(v);
    sb_free(&out);
    return 0;
}

int main(int argc, char **argv)
{
    long n, pos;
    unsigned char *ivf, head[32];
    vp8_decoder_t *src, *dec;
    vp8_encoder_t *enc = NULL;
    sb_t out = {0};
    FILE *fo, *fy;
    int frames = 0, bad = 0, kbps;
    double worst = 99, sum = 0;
    size_t bytes = 0;

    if (argc == 5 && strcmp(argv[1], "--speed") == 0)
        return speed(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (argc < 5 || !(ivf = load(argv[1], &n)) || n < 32 || !(fo = fopen(argv[3], "wb")) || !(fy = fopen(argv[4], "wb"))) {
        fprintf(stderr, "usage: vp8_roundtrip source.ivf kbps ours.ivf ours.yuv\n");
        return 2;
    }
    kbps = atoi(argv[2]);
    src = vp8_decoder_new();
    dec = vp8_decoder_new();
    memcpy(head, ivf, 32);
    fwrite(head, 1, 32, fo);
    for (pos = ivf[6] | ivf[7] << 8; pos + 12 <= n;) {
        unsigned size = ivf[pos] | ivf[pos + 1] << 8 | ivf[pos + 2] << 16 | (unsigned)ivf[pos + 3] << 24;
        vp8_image_t in, got, rec;
        unsigned char fh[12] = {0};
        pos += 12;
        if (pos + (long)size > n)
            break;
        if (vp8_decode(src, ivf + pos, size, &in) == 1) {
            double p;
            if (!enc)
                enc = vp8_encoder_new(in.w, in.h, kbps, 30);
            vp8_encode(enc, &in, 0, &out);
            put_le(fh, (unsigned)out.len, 4);
            put_le(fh + 4, (unsigned)frames, 4);
            fwrite(fh, 1, 12, fo);
            fwrite(out.data, 1, out.len, fo);
            bytes += out.len;
            vp8_encoder_recon(enc, &rec);
            if (vp8_decode(dec, (const unsigned char *)out.data, out.len, &got) != 1 || !same(&got, &rec)) {
                if (!bad)
                    printf("%s: frame %d decodes differently from the encoder's reconstruction\n", argv[1], frames);
                bad++;
            } else {
                write_yuv(fy, &got);
            }
            p = psnr(&in, &got);
            worst = p < worst ? p : worst;
            sum += p;
            frames++;
        }
        pos += size;
    }
    fclose(fo);
    fclose(fy);
    printf("%s: %d frames at %d kbit/s: %zu bytes, PSNR %.1f dB average, %.1f worst, %d mismatches\n", argv[1], frames,
           kbps, bytes, frames ? sum / frames : 0, worst, bad);
    vp8_encoder_free(enc);
    vp8_decoder_free(src);
    vp8_decoder_free(dec);
    sb_free(&out);
    free(ivf);
    return bad || worst < MIN_PSNR ? 1 : 0;
}
