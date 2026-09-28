#include <string.h>
#include "apng.h"
#include "inflate.h"
#include "mem.h"

#define MAX_SIDE 1024

struct apng {
    unsigned char *file;
    size_t n;
    unsigned w, h, frames, frame;
    int ctype, bpp;              /* 6 RGBA, 2 RGB, 3 palette; bytes per pixel */
    unsigned char pal[256][4];   /* RGBA */
    int key;                     /* RGB images: a transparent color is set */
    unsigned key_r, key_g, key_b;
    size_t first, pos;           /* first fcTL, next fcTL */
    int dispose;                 /* of the frame on the canvas: 0 none, 1 clear, 2 previous */
    unsigned ax, ay, aw, ah;     /* its area */
    unsigned char *saved;        /* the canvas before it, for dispose 2 */
};

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static unsigned be16(const unsigned char *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

/* The chunk at `o`: its type and data. Returns 0 past the end or when cut short. */
static int chunk(const apng_t *a, size_t o, const unsigned char **type, const unsigned char **data, unsigned *len)
{
    if (o + 12 > a->n)
        return 0;
    *len = be32(a->file + o);
    if (*len > a->n - o - 12)
        return 0;
    *type = a->file + o + 4;
    *data = a->file + o + 8;
    return 1;
}

static int is(const unsigned char *type, const char *name)
{
    return type[0] == name[0] && type[1] == name[1] && type[2] == name[2] && type[3] == name[3];
}

apng_t *apng_open(const void *data, size_t n)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    apng_t *a;
    const unsigned char *type, *d;
    unsigned len;
    size_t o = 8;
    int depth = 0, interlace = 1;

    if (n < 8 || n > (16u << 20))
        return NULL;
    for (int k = 0; k < 8; k++) /* no memcmp without the C runtime */
        if (((const unsigned char *)data)[k] != sig[k])
            return NULL;
    a = mem_alloc(sizeof *a);
    a->file = mem_alloc(n);
    memcpy(a->file, data, n);
    a->n = n;
    for (int k = 0; k < 256; k++)
        a->pal[k][3] = 255;
    while (chunk(a, o, &type, &d, &len)) {
        if (is(type, "IHDR") && len >= 13) {
            a->w = be32(d);
            a->h = be32(d + 4);
            depth = d[8];
            a->ctype = d[9];
            interlace = d[12];
        } else if (is(type, "acTL") && len >= 8) {
            a->frames = be32(d);
        } else if (is(type, "PLTE")) {
            for (unsigned k = 0; k < len / 3 && k < 256; k++) {
                a->pal[k][0] = d[k * 3];
                a->pal[k][1] = d[k * 3 + 1];
                a->pal[k][2] = d[k * 3 + 2];
            }
        } else if (is(type, "tRNS")) {
            if (a->ctype == 3) {
                for (unsigned k = 0; k < len && k < 256; k++)
                    a->pal[k][3] = d[k];
            } else if (a->ctype == 2 && len >= 6) {
                a->key = 1;
                a->key_r = be16(d);
                a->key_g = be16(d + 2);
                a->key_b = be16(d + 4);
            }
        } else if (is(type, "fcTL") && !a->first) {
            a->first = o;
        } else if (is(type, "IEND")) {
            break;
        }
        o += 12 + (size_t)len;
    }
    a->bpp = a->ctype == 6 ? 4 : a->ctype == 2 ? 3 : 1;
    if (depth != 8 || (a->ctype != 2 && a->ctype != 3 && a->ctype != 6) || interlace || a->frames < 2 || !a->first ||
        !a->w || !a->h || a->w > MAX_SIDE || a->h > MAX_SIDE) {
        apng_free(a);
        return NULL;
    }
    return a;
}

void apng_size(const apng_t *a, unsigned *w, unsigned *h)
{
    *w = a->w;
    *h = a->h;
}

unsigned apng_frames(const apng_t *a)
{
    return a->frames;
}

size_t apng_bytes(const apng_t *a)
{
    return sizeof *a + a->n + (a->saved ? (size_t)a->w * a->h * 4 : 0);
}

void apng_free(apng_t *a)
{
    if (!a)
        return;
    mem_free(a->file);
    mem_free(a->saved);
    mem_free(a);
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;

    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undoes the row filters in place; rows are 1 filter byte then `stride` bytes. */
static int unfilter(unsigned char *p, unsigned rows, unsigned stride, int bpp)
{
    const unsigned char *prev = NULL;

    for (unsigned y = 0; y < rows; y++, p += stride + 1) {
        unsigned char *cur = p + 1;
        int type = p[0];
        if (type > 4)
            return 0;
        for (unsigned i = 0; i < stride; i++) {
            int a = i >= (unsigned)bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0,
                c = prev && i >= (unsigned)bpp ? prev[i - bpp] : 0;
            int add = type == 1 ? a : type == 2 ? b : type == 3 ? (a + b) / 2 : type == 4 ? paeth(a, b, c) : 0;
            cur[i] = (unsigned char)(cur[i] + add);
        }
        prev = cur;
    }
    return 1;
}

unsigned apng_next(apng_t *a, unsigned char *canvas)
{
    const unsigned char *type, *d;
    unsigned len, fw, fh, fx, fy, dnum, dden, dop, bop, stride, ms = 0;
    size_t o, row = (size_t)a->w * 4;
    sb_t packed = {0}, raw = {0};
    inflate_t *z;

    if (a->frame == 0) {
        a->pos = a->first;
        a->dispose = 0;
        memset(canvas, 0, row * a->h);
    } else if (a->dispose == 1) {
        for (unsigned y = a->ay; y < a->ay + a->ah; y++)
            memset(canvas + y * row + (size_t)a->ax * 4, 0, (size_t)a->aw * 4);
    } else if (a->dispose == 2 && a->saved) {
        for (unsigned y = a->ay; y < a->ay + a->ah; y++)
            memcpy(canvas + y * row + (size_t)a->ax * 4, a->saved + y * row + (size_t)a->ax * 4, (size_t)a->aw * 4);
    }
    if (!chunk(a, a->pos, &type, &d, &len) || !is(type, "fcTL") || len < 26)
        return 0;
    fw = be32(d + 4);
    fh = be32(d + 8);
    fx = be32(d + 12);
    fy = be32(d + 16);
    dnum = be16(d + 20);
    dden = be16(d + 22);
    dop = d[24];
    bop = d[25];
    if (!fw || !fh || fx > a->w || fy > a->h || fw > a->w - fx || fh > a->h - fy)
        return 0;
    /* Its image data: IDAT for a frame that is also the default image, fdAT (after a sequence number) otherwise. */
    for (o = a->pos + 12 + len; chunk(a, o, &type, &d, &len); o += 12 + (size_t)len) {
        if (is(type, "fcTL") || is(type, "IEND"))
            break;
        if (is(type, "IDAT"))
            sb_addn(&packed, (const char *)d, len);
        else if (is(type, "fdAT") && len >= 4)
            sb_addn(&packed, (const char *)d + 4, len - 4);
    }
    a->pos = o;
    stride = fw * (unsigned)a->bpp;
    z = mem_alloc(sizeof *z);
    inflate_init(z);
    if (packed.len && inflate_message(z, (const unsigned char *)packed.data, packed.len, &raw) &&
        raw.len >= (size_t)fh * (stride + 1) && unfilter((unsigned char *)raw.data, fh, stride, a->bpp)) {
        if (a->frame == 0 && dop == 2)
            dop = 1; /* nothing before the first frame to go back to */
        if (dop == 2) {
            if (!a->saved)
                a->saved = mem_alloc(row * a->h);
            memcpy(a->saved, canvas, row * a->h);
        }
        for (unsigned y = 0; y < fh; y++) {
            const unsigned char *s = (const unsigned char *)raw.data + (size_t)y * (stride + 1) + 1;
            unsigned char *px = canvas + (fy + y) * row + (size_t)fx * 4;
            for (unsigned x = 0; x < fw; x++, px += 4) {
                unsigned r, g, b, al;
                if (a->ctype == 3) {
                    const unsigned char *p = a->pal[s[x]];
                    r = p[0], g = p[1], b = p[2], al = p[3];
                } else {
                    const unsigned char *p = s + x * (unsigned)a->bpp;
                    r = p[0], g = p[1], b = p[2];
                    al = a->ctype == 6 ? p[3] : a->key && r == a->key_r && g == a->key_g && b == a->key_b ? 0 : 255;
                }
                r = r * al / 255, g = g * al / 255, b = b * al / 255; /* premultiplied, as the renderer draws */
                if (bop == 1 && al < 255) {
                    px[0] = (unsigned char)(b + px[0] * (255 - al) / 255);
                    px[1] = (unsigned char)(g + px[1] * (255 - al) / 255);
                    px[2] = (unsigned char)(r + px[2] * (255 - al) / 255);
                    px[3] = (unsigned char)(al + px[3] * (255 - al) / 255);
                } else {
                    px[0] = (unsigned char)b, px[1] = (unsigned char)g, px[2] = (unsigned char)r, px[3] = (unsigned char)al;
                }
            }
        }
        a->dispose = (int)dop;
        a->ax = fx, a->ay = fy, a->aw = fw, a->ah = fh;
        ms = dnum * 1000 / (dden ? dden : 100);
        if (ms < 10)
            ms = 100; /* as browsers do for "as fast as possible" */
        a->frame = (a->frame + 1) % a->frames;
    }
    mem_free(z);
    sb_free(&packed);
    sb_free(&raw);
    return ms;
}
