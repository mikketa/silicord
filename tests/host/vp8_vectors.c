/*
 * Conformance check against libvpx's VP8 test vectors:
 *
 *   vp8_vectors vp80-00-comprehensive-001.ivf vp80-00-comprehensive-001.ivf.md5
 *
 * Decodes the IVF file and compares the MD5 of every shown frame (its
 * I420 planes, cropped to the picture size) with the reference list.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vp8.h"

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
    if (buf)
        buf[*n] = 0;
    return buf;
}

/* ---- MD5 (RFC 1321) ---- */

typedef struct {
    unsigned a, b, c, d;
    unsigned long long len;
    unsigned char buf[64];
    int n;
} md5_t;

static unsigned rol(unsigned x, int s)
{
    return x << s | x >> (32 - s);
}

static void md5_block(md5_t *m, const unsigned char *p)
{
    static const unsigned char s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                        14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                        4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    /* floor(|sin(i + 1)| * 2^32) */
    static const unsigned k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    unsigned w[16], a = m->a, b = m->b, c = m->c, d = m->d;

    for (int i = 0; i < 16; i++)
        w[i] = (unsigned)p[4 * i] | (unsigned)p[4 * i + 1] << 8 | (unsigned)p[4 * i + 2] << 16 | (unsigned)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        unsigned f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        f += a + k[i] + w[g];
        a = d;
        d = c;
        c = b;
        b += rol(f, s[i]);
    }
    m->a += a;
    m->b += b;
    m->c += c;
    m->d += d;
}

static void md5_init(md5_t *m)
{
    memset(m, 0, sizeof *m);
    m->a = 0x67452301;
    m->b = 0xefcdab89;
    m->c = 0x98badcfe;
    m->d = 0x10325476;
}

static void md5_add(md5_t *m, const unsigned char *p, size_t n)
{
    m->len += n;
    while (n--) {
        m->buf[m->n++] = *p++;
        if (m->n == 64) {
            md5_block(m, m->buf);
            m->n = 0;
        }
    }
}

static void md5_hex(md5_t *m, char *out)
{
    unsigned long long bits = m->len * 8;
    unsigned char pad = 0x80, zero = 0, len[8];
    unsigned v[4];

    md5_add(m, &pad, 1);
    while (m->n != 56)
        md5_add(m, &zero, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (unsigned char)(bits >> (8 * i));
    md5_add(m, len, 8);
    v[0] = m->a;
    v[1] = m->b;
    v[2] = m->c;
    v[3] = m->d;
    for (int i = 0; i < 16; i++)
        sprintf(out + 2 * i, "%02x", (v[i / 4] >> (8 * (i % 4))) & 255);
}

static void frame_md5(const vp8_image_t *img, char *out)
{
    md5_t m;
    int cw = (img->w + 1) / 2, ch = (img->h + 1) / 2;

    md5_init(&m);
    for (int y = 0; y < img->h; y++)
        md5_add(&m, img->y + y * img->y_stride, (size_t)img->w);
    for (int y = 0; y < ch; y++)
        md5_add(&m, img->u + y * img->uv_stride, (size_t)cw);
    for (int y = 0; y < ch; y++)
        md5_add(&m, img->v + y * img->uv_stride, (size_t)cw);
    md5_hex(&m, out);
}

int main(int argc, char **argv)
{
    long n, md5_n;
    unsigned char *ivf, *list;
    const char *line;
    vp8_decoder_t *d;
    long pos;
    int frames = 0, shown = 0, bad = 0, first_bad = -1;

    if (argc < 3 || !(ivf = load(argv[1], &n)) || !(list = load(argv[2], &md5_n)) || n < 32 || memcmp(ivf, "DKIF", 4)) {
        fprintf(stderr, "usage: vp8_vectors file.ivf file.ivf.md5\n");
        return 2;
    }
    d = vp8_decoder_new();
    line = (const char *)list;
    pos = ivf[6] | ivf[7] << 8; /* the header's length */
    for (; pos + 12 <= n; frames++) {
        unsigned size = ivf[pos] | ivf[pos + 1] << 8 | ivf[pos + 2] << 16 | (unsigned)ivf[pos + 3] << 24;
        vp8_image_t img;
        int r;
        pos += 12;
        if (pos + (long)size > n)
            break;
        r = vp8_decode(d, ivf + pos, size, &img);
        pos += size;
        if (r < 0) {
            printf("%s: frame %d does not decode\n", argv[1], frames);
            bad++;
            if (first_bad < 0)
                first_bad = frames;
            continue;
        }
        if (r == 1) {
            char got[33];
            frame_md5(&img, got);
            if (!*line || strncmp(line, got, 32) != 0) {
                bad++;
                if (first_bad < 0)
                    first_bad = frames;
            }
            shown++;
            line = strchr(line, '\n');
            line = line ? line + 1 : "";
        }
    }
    /* reference frames left over count as a mismatch */
    if (*line)
        bad++;
    printf("%s: %d frames, %d shown, %d mismatches (first at %d)\n", argv[1], frames, shown, bad, first_bad);
    vp8_decoder_free(d);
    free(ivf);
    free(list);
    return bad ? 1 : 0;
}
