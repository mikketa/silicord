/* Video pictures to BGRA at the size they are shown: exact conversion, box shrinking, area and bilinear resampling. */
#include "test.h"
#include "mem.h"
#include "picture.h"

static unsigned g_seed = 12345;

static int rnd(int n)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (int)((g_seed >> 8) % (unsigned)n);
}

/* A w x h picture with padded strides, filled by fill(x, y, plane) (0 luma, 1 U, 2 V). */
typedef struct {
    vp8_image_t img;
    unsigned char *mem;
} pic_t;

static void pic_make(pic_t *p, int w, int h, int (*fill)(int x, int y, int plane))
{
    int cw = (w + 1) / 2, ch = (h + 1) / 2, ys = w + 5, cs = cw + 3;
    unsigned char *y, *u, *v;

    p->mem = mem_alloc((size_t)ys * h + 2 * (size_t)cs * ch);
    y = p->mem;
    u = y + (size_t)ys * h;
    v = u + (size_t)cs * ch;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            y[r * ys + c] = (unsigned char)fill(c, r, 0);
    for (int r = 0; r < ch; r++)
        for (int c = 0; c < cw; c++) {
            u[r * cs + c] = (unsigned char)fill(c, r, 1);
            v[r * cs + c] = (unsigned char)fill(c, r, 2);
        }
    p->img.w = w;
    p->img.h = h;
    p->img.y = y;
    p->img.u = u;
    p->img.v = v;
    p->img.y_stride = ys;
    p->img.uv_stride = cs;
}

static int clamp(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* BT.601 video range, one pixel, as the conversion's formula. */
static unsigned ref_bgra(int y, int u, int v)
{
    int c = 298 * (y - 16), d = u - 128, e = v - 128;

    return 0xFF000000u | (unsigned)clamp((c + 409 * e + 128) >> 8) << 16 |
           (unsigned)clamp((c - 100 * d - 208 * e + 128) >> 8) << 8 | (unsigned)clamp((c + 516 * d + 128) >> 8);
}

static int ref_box(const unsigned char *p, int stride, int n)
{
    int sum = n * n / 2;

    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++)
            sum += p[r * stride + c];
    return sum / (n * n);
}

static int fill_random(int x, int y, int plane)
{
    (void)x;
    (void)y;
    (void)plane;
    return rnd(256);
}

static int fill_flat(int x, int y, int plane)
{
    (void)x;
    (void)y;
    return plane == 0 ? 140 : plane == 1 ? 90 : 170;
}

static int fill_ramp(int x, int y, int plane)
{
    (void)y;
    return plane == 0 ? 16 + x * 219 / 200 : 128;
}

static int channel(unsigned px, int c)
{
    return (int)(px >> (c * 8) & 0xFF);
}

void entry(void)
{
    picture_scratch_t scratch = {0};
    unsigned *out = mem_alloc(700 * 400 * 4);
    pic_t p;
    int w, h, ok;

    picture_fit(1280, 720, 400, 400, &w, &h);
    check(w == 400 && h == 225, "fit: a wide picture fills the width");
    picture_fit(720, 1280, 400, 400, &w, &h);
    check(w == 225 && h == 400, "fit: a tall one the height");
    picture_fit(640, 360, 1, 1, &w, &h);
    check(w == 1 && h == 1, "fit: never below one pixel");
    picture_fit(1280, 720, 421, 300, &w, &h);
    {
        int w2, h2;
        picture_fit(w, h, 421, 300, &w2, &h2);
        check(w2 == w && h2 == h, "fit: a fitted picture fits as it is, so the tile draws it by copying");
    }

    /* At its own size: exactly the formula, pixel by pixel. */
    pic_make(&p, 37, 23, fill_random);
    picture_to_bgra(&p.img, 37, 23, out, &scratch);
    ok = 1;
    for (int y = 0; y < 23; y++)
        for (int x = 0; x < 37; x++) {
            int c = (y / 2) * p.img.uv_stride + x / 2;
            ok &= out[y * 37 + x] == ref_bgra(p.img.y[y * p.img.y_stride + x], p.img.u[c], p.img.v[c]);
        }
    check(ok, "own size: each pixel converted exactly");
    mem_free(p.mem);

    /* By 2 and 4: the rounded averages of the boxes, in YUV. */
    pic_make(&p, 67, 41, fill_random);
    for (int s = 2; s <= 4; s *= 2) {
        int c = s / 2;
        picture_to_bgra(&p.img, 67 / s, 41 / s, out, &scratch);
        ok = 1;
        for (int y = 0; y < 41 / s; y++)
            for (int x = 0; x < 67 / s; x++)
                ok &= out[y * (67 / s) + x] ==
                      ref_bgra(ref_box(p.img.y + y * s * p.img.y_stride + x * s, p.img.y_stride, s),
                               ref_box(p.img.u + y * c * p.img.uv_stride + x * c, p.img.uv_stride, c),
                               ref_box(p.img.v + y * c * p.img.uv_stride + x * c, p.img.uv_stride, c));
        check(ok, s == 2 ? "half size: boxes of 2 x 2" : "quarter size: boxes of 4 x 4");
    }
    mem_free(p.mem);

    /* Any size, up or down: the weights add up to one, so a flat color stays exactly that color. */
    pic_make(&p, 101, 57, fill_flat);
    {
        static const int sizes[][2] = {{101, 57}, {37, 21}, {250, 140}, {1, 1}, {101, 3},
                                       {3, 57},   {640, 360}, {60, 34}, {26, 15}};
        unsigned flat = ref_bgra(140, 90, 170);
        ok = 1;
        for (int k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
            picture_to_bgra(&p.img, sizes[k][0], sizes[k][1], out, &scratch);
            for (int i = 0; i < sizes[k][0] * sizes[k][1]; i++)
                ok &= out[i] == flat;
        }
        check(ok, "any size: a flat color stays flat");
    }
    mem_free(p.mem);

    /* A ramp stays a ramp: every row the same, never going down, from its darkest to its lightest. */
    pic_make(&p, 200, 30, fill_ramp);
    {
        static const int sizes[][2] = {{150, 20}, {90, 11}, {31, 5}, {333, 50}, {613, 91}};
        ok = 1;
        for (int k = 0; k < 5; k++) {
            int sw = sizes[k][0], sh = sizes[k][1];
            picture_to_bgra(&p.img, sw, sh, out, &scratch);
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++) {
                    ok &= out[y * sw + x] == out[x];
                    if (x)
                        ok &= channel(out[x], 1) >= channel(out[x - 1], 1);
                }
            ok &= channel(out[sw - 1], 1) - channel(out[0], 1) > 200;
        }
        check(ok, "a ramp stays a ramp at any size");
    }
    mem_free(p.mem);

    /* Shrinking three columns to two: each takes one and a half, 2/3 and 1/3 of its pixels (within a rounding). */
    {
        static const int luma[3] = {40, 200, 120};
        unsigned a, b, c;
        pic_make(&p, 3, 1, fill_flat);
        for (int x = 0; x < 3; x++)
            ((unsigned char *)p.img.y)[x] = (unsigned char)luma[x];
        picture_to_bgra(&p.img, 3, 1, out, &scratch);
        a = out[0];
        b = out[1];
        c = out[2];
        picture_to_bgra(&p.img, 2, 1, out, &scratch);
        ok = 1;
        for (int ch = 0; ch < 3; ch++) {
            int left = (2 * channel(a, ch) + channel(b, ch) + 1) / 3;
            int right = (channel(b, ch) + 2 * channel(c, ch) + 1) / 3;
            ok &= channel(out[0], ch) - left <= 1 && left - channel(out[0], ch) <= 1;
            ok &= channel(out[1], ch) - right <= 1 && right - channel(out[1], ch) <= 1;
        }
        check(ok, "area: each pixel weighted by how much of it is covered");
        mem_free(p.mem);
    }

    /* Enlarging twice: the pixel centers fall between the source ones, a quarter and three quarters. */
    {
        unsigned a, b;
        pic_make(&p, 2, 1, fill_flat);
        ((unsigned char *)p.img.y)[0] = 60;
        ((unsigned char *)p.img.y)[1] = 220;
        picture_to_bgra(&p.img, 2, 1, out, &scratch);
        a = out[0];
        b = out[1];
        picture_to_bgra(&p.img, 4, 1, out, &scratch);
        ok = out[0] == a && out[3] == b; /* clamped at the edges */
        for (int ch = 0; ch < 3; ch++) {
            int q1 = (3 * channel(a, ch) + channel(b, ch) + 2) / 4, q3 = (channel(a, ch) + 3 * channel(b, ch) + 2) / 4;
            ok &= channel(out[1], ch) - q1 <= 1 && q1 - channel(out[1], ch) <= 1;
            ok &= channel(out[2], ch) - q3 <= 1 && q3 - channel(out[2], ch) <= 1;
        }
        check(ok, "bilinear: between the two nearest pixel centers");
        mem_free(p.mem);
    }

    /* Random sizes, shrinking and enlarging, through one scratch: opaque everywhere (the sanitizers watch the rest). */
    ok = 1;
    for (int k = 0; k < 200; k++) {
        int iw = 1 + rnd(300), ih = 1 + rnd(200), ow = 1 + rnd(700), oh = 1 + rnd(400);
        pic_make(&p, iw, ih, fill_random);
        for (int i = 0; i < ow * oh; i++)
            out[i] = 0;
        picture_to_bgra(&p.img, ow, oh, out, &scratch);
        for (int i = 0; i < ow * oh; i++)
            ok &= out[i] >> 24 == 0xFF;
        mem_free(p.mem);
    }
    check(ok, "random sizes: every pixel written, opaque");

    /* BGRA to I420, against the formulas written out, at sizes with SIMD bodies and odd edges. */
    {
        static const int sizes[][2] = {{37, 23}, {64, 16}, {1, 1}, {9, 3}, {17, 2}, {8, 1}};
        unsigned char *yp = mem_alloc(80 * 30), *up = mem_alloc(40 * 15), *vp = mem_alloc(40 * 15);
        ok = 1;
        for (int k = 0; k < 6; k++) {
            int iw = sizes[k][0], ih = sizes[k][1], stride = iw + 3;
            for (int i = 0; i < stride * ih; i++)
                out[i] = (unsigned)rnd(256) | (unsigned)rnd(256) << 8 | (unsigned)rnd(256) << 16 | 0xFF000000u;
            picture_bgra_to_i420(out, iw, ih, stride, yp, up, vp, 80, 40);
            for (int y = 0; y < ih; y++)
                for (int x = 0; x < iw; x++) {
                    unsigned px = out[y * stride + x];
                    int r = (int)(px >> 16 & 0xFF), g = (int)(px >> 8 & 0xFF), b = (int)(px & 0xFF);
                    ok &= yp[y * 80 + x] == ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                }
            for (int y = 0; y < (ih + 1) / 2; y++)
                for (int x = 0; x < (iw + 1) / 2; x++) {
                    int sum[3] = {2, 2, 2}, cu, cv;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            int sx = 2 * x + dx < iw ? 2 * x + dx : iw - 1, sy = 2 * y + dy < ih ? 2 * y + dy : ih - 1;
                            unsigned px = out[sy * stride + sx];
                            for (int c = 0; c < 3; c++)
                                sum[c] += (int)(px >> (8 * c) & 0xFF);
                        }
                    for (int c = 0; c < 3; c++)
                        sum[c] >>= 2;
                    cu = ((-38 * sum[2] - 74 * sum[1] + 112 * sum[0] + 128) >> 8) + 128;
                    cv = ((112 * sum[2] - 94 * sum[1] - 18 * sum[0] + 128) >> 8) + 128;
                    ok &= up[y * 40 + x] == cu && vp[y * 40 + x] == cv;
                }
        }
        check(ok, "to I420: every sample as the formulas give it, odd edges doubled");

        for (int i = 0; i < 16; i++)
            out[i] = i < 8 ? 0xFFFFFFFFu : 0xFF000000u;
        picture_bgra_to_i420(out, 16, 1, 16, yp, up, vp, 80, 40);
        check(yp[0] == 235 && yp[15] == 16 && up[0] == 128 && vp[0] == 128 && up[7] == 128 && vp[7] == 128,
              "to I420: white is 235, black 16, both without color");

        /* There and back: colors RGB can hold, chroma flat over each 2 x 2 box, come back within a rounding or two. */
        pic_make(&p, 40, 24, fill_random);
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 40; x++)
                ((unsigned char *)p.img.y)[y * p.img.y_stride + x] = (unsigned char)(60 + (x * 7 + y * 3) % 120);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 20; x++) {
                ((unsigned char *)p.img.u)[y * p.img.uv_stride + x] = (unsigned char)(110 + (x + y) % 36);
                ((unsigned char *)p.img.v)[y * p.img.uv_stride + x] = (unsigned char)(115 + (x * 2 + y) % 30);
            }
        picture_to_bgra(&p.img, 40, 24, out, &scratch);
        picture_bgra_to_i420(out, 40, 24, 40, yp, up, vp, 80, 40);
        ok = 1;
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 40; x++) {
                int d = yp[y * 80 + x] - p.img.y[y * p.img.y_stride + x];
                ok &= d >= -2 && d <= 2;
            }
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 20; x++) {
                int du = up[y * 40 + x] - p.img.u[y * p.img.uv_stride + x], dv = vp[y * 40 + x] - p.img.v[y * p.img.uv_stride + x];
                ok &= du >= -3 && du <= 3 && dv >= -3 && dv <= 3;
            }
        check(ok, "YUV to BGRA and back: within a rounding or two");
        mem_free(p.mem);
        mem_free(yp);
        mem_free(up);
        mem_free(vp);
    }

    /* Scaling BGRA with rows apart: flat stays flat, its own size is a copy, and nothing past a row is read. */
    {
        unsigned *src = mem_alloc(100 * 60 * 4);
        for (int i = 0; i < 100 * 60; i++)
            src[i] = (i % 100) < 90 ? 0xFF336699u : 0x12345678u; /* the last 10 of each row are outside the picture */
        ok = 1;
        picture_scale_bgra(src, 90, 60, 100, 45, 30, out, &scratch);
        for (int i = 0; i < 45 * 30; i++)
            ok &= out[i] == 0xFF336699u;
        picture_scale_bgra(src, 90, 60, 100, 131, 77, out, &scratch);
        for (int i = 0; i < 131 * 77; i++)
            ok &= out[i] == 0xFF336699u;
        picture_scale_bgra(src, 90, 60, 100, 33, 59, out, &scratch);
        for (int i = 0; i < 33 * 59; i++)
            ok &= out[i] == 0xFF336699u;
        picture_scale_bgra(src, 90, 60, 100, 90, 60, out, &scratch);
        for (int i = 0; i < 90 * 60; i++)
            ok &= out[i] == 0xFF336699u;
        check(ok, "BGRA scaling: rows apart, flat colors exact, own size copied");
        mem_free(src);
    }

    /* The mouse pointer, as the three kinds of shape Windows gives, clipped at the edges. */
    {
        static const unsigned char mono[4] = {0x3F, 0xFF, 0x80, 0x00}; /* 8 x 2, a byte a row: AND rows, then XOR rows */
        static const unsigned color[4] = {0xFF0000FFu, 0x800000FFu, 0x00FFFFFFu, 0xFFFFFFFFu};
        static const unsigned masked[2] = {0x00123456u, 0xFF00FF00u};
        unsigned bg = 0xFF404040u, half_b = (0xFF * 0x80 + 0x40 * 0x7F + 127) / 255, half_gr = (0x40 * 0x7F + 127) / 255;
        for (int i = 0; i < 16 * 4; i++)
            out[i] = bg;
        picture_draw_cursor(out, 16, 4, 16, 2, 0, PICTURE_CURSOR_MONOCHROME, mono, 8, 4, 1);
        /* row 0: the AND mask clears the first two pixels, the XOR mask turns the first white; row 1 keeps the screen */
        ok = out[2] == 0xFFFFFFFFu && out[3] == 0xFF000000u && out[4] == bg && out[16 + 2] == bg && out[16 + 9] == bg;
        picture_draw_cursor(out, 16, 4, 16, 14, 2, PICTURE_CURSOR_COLOR, (const unsigned char *)color, 2, 2, 8);
        ok &= out[2 * 16 + 14] == 0xFF0000FFu && out[3 * 16 + 14] == bg && out[3 * 16 + 15] == 0xFFFFFFFFu;
        ok &= out[2 * 16 + 15] == (0xFF000000u | half_gr << 16 | half_gr << 8 | half_b);
        picture_draw_cursor(out, 16, 4, 16, -1, 3, PICTURE_CURSOR_MASKED, (const unsigned char *)masked, 2, 1, 8);
        ok &= out[3 * 16] == (bg ^ 0x00FF00u); /* alpha set: inverts; the other pixel is off the picture */
        check(ok, "pointer: monochrome, color (blended by alpha) and masked shapes, clipped");
    }

    picture_scratch_free(&scratch);
    mem_free(out);
    finish();
}
