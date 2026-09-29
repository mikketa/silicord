#include <emmintrin.h>
#include <string.h>
#include "picture.h"
#include "mem.h"

void picture_fit(int pw, int ph, int tw, int th, int *w, int *h)
{
    *w = tw;
    *h = th;
    if (pw > 0 && ph > 0) {
        if ((long long)pw * th > (long long)ph * tw)
            *h = (int)((long long)tw * ph / pw);
        else
            *w = (int)((long long)th * pw / ph);
    }
    if (*w < 1)
        *w = 1;
    if (*h < 1)
        *h = 1;
}

/* ---- YUV to BGRA ---- */

/* R, G and B of 8 pixels, 16-bit Y - 16 and the chroma d = U - 128, e = V - 128 of each, clamped to bytes. */
static __m128i yuv_channel(__m128i y, __m128i a, __m128i b, __m128i k)
{
    /* k pairs a factor for y with one for a, then one for b with the rounding. */
    __m128i one = _mm_set1_epi16(1), kya = _mm_shuffle_epi32(k, 0x00), kb1 = _mm_shuffle_epi32(k, 0xAA);
    __m128i lo = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(y, a), kya),
                               _mm_madd_epi16(_mm_unpacklo_epi16(b, one), kb1));
    __m128i hi = _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(y, a), kya),
                               _mm_madd_epi16(_mm_unpackhi_epi16(b, one), kb1));

    lo = _mm_packs_epi32(_mm_srai_epi32(lo, 8), _mm_srai_epi32(hi, 8));
    return _mm_packus_epi16(lo, lo);
}

/* Two 16-bit values in each 32-bit lane, a low. */
static int pair16(int a, int b)
{
    return (int)((unsigned)(unsigned short)a | (unsigned)(unsigned short)b << 16);
}

/*
 * Eight pixels to BGRA: the products summed in 32 bits by pmaddwd,
 * (sum + 128) >> 8 and clamped by the packs, the same integers as
 * bgra1() pixel by pixel.
 */
static void bgra8(unsigned *o, __m128i y, __m128i d, __m128i e)
{
    const __m128i kr = _mm_set_epi32(0, pair16(0, 128), 0, pair16(298, 409)); /* 298 y + 409 e + 0 d + 128 */
    const __m128i kg = _mm_set_epi32(0, pair16(-208, 128), 0, pair16(298, -100));
    const __m128i kb = _mm_set_epi32(0, pair16(0, 128), 0, pair16(298, 516));
    __m128i bg = _mm_unpacklo_epi8(yuv_channel(y, d, d, kb), yuv_channel(y, d, e, kg));
    __m128i ra = _mm_unpacklo_epi8(yuv_channel(y, e, d, kr), _mm_set1_epi8(-1));

    _mm_storeu_si128((__m128i *)o, _mm_unpacklo_epi16(bg, ra));
    _mm_storeu_si128((__m128i *)(o + 4), _mm_unpackhi_epi16(bg, ra));
}

static unsigned char clamp8(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static unsigned bgra1(int y, int d, int e)
{
    int c = 298 * (y - 16);

    return 0xFF000000u | (unsigned)clamp8((c + 409 * e + 128) >> 8) << 16 |
           (unsigned)clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8 | clamp8((c + 516 * d + 128) >> 8);
}

/* Eight 16-bit lanes, each the sum of n (1, 2 or 4) bytes in a row from p. */
static __m128i sum_across(const unsigned char *p, int n)
{
    __m128i low = _mm_set1_epi16(0xFF), a, b;

    if (n == 1)
        return _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)p), _mm_setzero_si128());
    a = _mm_loadu_si128((const __m128i *)p);
    a = _mm_add_epi16(_mm_and_si128(a, low), _mm_srli_epi16(a, 8));
    if (n == 2)
        return a;
    b = _mm_loadu_si128((const __m128i *)(p + 16));
    b = _mm_add_epi16(_mm_and_si128(b, low), _mm_srli_epi16(b, 8));
    return _mm_packs_epi32(_mm_madd_epi16(a, _mm_set1_epi16(1)), _mm_madd_epi16(b, _mm_set1_epi16(1)));
}

/* The rounded averages of eight n x n boxes side by side from p (n = 1, 2 or 4), in 16-bit lanes. */
static __m128i box8(const unsigned char *p, int stride, int n)
{
    __m128i sum = _mm_setzero_si128();

    for (int r = 0; r < n; r++)
        sum = _mm_add_epi16(sum, sum_across(p + r * stride, n));
    sum = _mm_add_epi16(sum, _mm_set1_epi16((short)(n * n / 2)));
    return _mm_srl_epi16(sum, _mm_cvtsi32_si128(n == 4 ? 4 : n == 2 ? 2 : 0));
}

/* The rounded average of an n x n box from p. */
static int box1(const unsigned char *p, int stride, int n)
{
    int sum = n * n / 2;

    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++)
            sum += p[r * stride + c];
    return sum / (n * n);
}

/*
 * The picture shrunk by s (1, 2 or 4) to (w / s) x (h / s): each pixel
 * the average of an s x s box of luma and of the chroma under it.
 */
static void convert(const vp8_image_t *img, int s, unsigned *out)
{
    const __m128i bias_y = _mm_set1_epi16(16), bias_c = _mm_set1_epi16(128), zero = _mm_setzero_si128();
    int w = img->w / s, h = img->h / s, c = s / 2; /* c: the chroma box, 0 when a sample covers two pixels */

    for (int y = 0; y < h; y++) {
        const unsigned char *py = img->y + y * s * img->y_stride;
        const unsigned char *pu = img->u + (c ? y * c : y >> 1) * img->uv_stride;
        const unsigned char *pv = img->v + (c ? y * c : y >> 1) * img->uv_stride;
        unsigned *o = out + (size_t)y * (size_t)w;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m128i d, e;
            if (c) {
                d = box8(pu + x * c, img->uv_stride, c);
                e = box8(pv + x * c, img->uv_stride, c);
            } else {
                /* Each chroma sample covers two pixels. */
                int u4, v4;
                __m128i u, v;
                memcpy(&u4, pu + (x >> 1), 4);
                memcpy(&v4, pv + (x >> 1), 4);
                u = _mm_cvtsi32_si128(u4);
                v = _mm_cvtsi32_si128(v4);
                d = _mm_unpacklo_epi8(_mm_unpacklo_epi8(u, u), zero);
                e = _mm_unpacklo_epi8(_mm_unpacklo_epi8(v, v), zero);
            }
            bgra8(o + x, _mm_sub_epi16(box8(py + x * s, img->y_stride, s), bias_y), _mm_sub_epi16(d, bias_c),
                  _mm_sub_epi16(e, bias_c));
        }
        for (; x < w; x++) {
            int d = c ? box1(pu + x * c, img->uv_stride, c) : pu[x >> 1];
            int e = c ? box1(pv + x * c, img->uv_stride, c) : pv[x >> 1];
            o[x] = bgra1(box1(py + x * s, img->y_stride, s), d - 128, e - 128);
        }
    }
}

/* ---- Resampling ---- */

/*
 * Along one axis, each destination pixel is a weighted sum of `taps`
 * consecutive source pixels from its first, the weights in 1/16384 summing
 * to 16384, so a flat color stays exactly that color. Every pixel takes
 * as many, zero weights filling in, so the loops have no cases.
 */
#define ONE 16384

/* The source pixels a destination pixel takes from n, going to m. */
static int axis_taps(int n, int m)
{
    int t = n > m ? (n + m - 1) / m + 1 : 2;

    return t < n ? t : n;
}

/* The weights kept per pixel: taps rounded up to even, read in pairs. */
static int axis_stride(int taps)
{
    return (taps + 1) & ~1;
}

/*
 * Shrinking, the source pixels under a destination pixel weighted by how
 * much of them it covers; enlarging, bilinear between the two nearest
 * pixel centers, as the renderer draws. Fills first[m] and, for each
 * pixel, its weights by pairs as a madd takes them (w, the weights of one
 * pixel, is scratch). Taps that would pass the edge move left, the
 * weights right.
 */
static void axis_weights(int n, int m, int taps, int *first, __m128i *k, short *w)
{
    int st = axis_stride(taps);

    for (int o = 0; o < m; o++) {
        int f, shift;
        for (int t = 0; t < st; t++)
            w[t] = 0;
        if (n > m) {
            /* In units of 1 / m of a source pixel: pixel i spans [i m, i m + m), the destination one [o n, o n + n). */
            int lo = o * n, hi = lo + n, count, sum = 0, big;
            f = lo / m;
            count = (hi + m - 1) / m - f;
            shift = f > n - taps ? f - (n - taps) : 0;
            big = shift;
            for (int t = 0; t < count; t++) {
                int a = (f + t) * m, b = a + m;
                int cover = (b < hi ? b : hi) - (a > lo ? a : lo);
                w[shift + t] = (short)(((long long)cover * ONE + n / 2) / n);
                sum += w[shift + t];
                if (w[shift + t] > w[big])
                    big = shift + t;
            }
            w[big] = (short)(w[big] + ONE - sum);
        } else {
            /* u = (o + 1/2) n / m - 1/2 in 1/16384 of a pixel, clamped to the picture. */
            long long u = (long long)(2 * o + 1) * n * ONE / (2 * m) - ONE / 2;
            int frac;
            if (u < 0)
                u = 0;
            f = (int)(u / ONE);
            frac = (int)(u - (long long)f * ONE);
            if (f >= n - 1) {
                f = n - 1;
                frac = 0;
            }
            shift = f > n - taps ? f - (n - taps) : 0;
            w[shift] = (short)(ONE - frac);
            if (shift + 1 < taps)
                w[shift + 1] = (short)frac;
        }
        first[o] = f - shift;
        for (int t = 0; t < st; t += 2)
            *k++ = _mm_set1_epi32((int)((unsigned)(unsigned short)w[t] | (unsigned)(unsigned short)w[t + 1] << 16));
    }
}

/*
 * One source row across: m pixels of four 16-bit channels, each value
 * times 128 (up to 32,640). Two source pixels a madd, their channels paired
 * (B B G G R R A A): with an odd number of taps the last pair reads one
 * pixel more, weighted 0, which the picture's slack holds.
 */
static void pass_across(const unsigned char *src, int m, const int *first, const __m128i *k, int pairs, short *mid)
{
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi32(64);

    for (int o = 0; o < m; o++) {
        const unsigned char *p = src + (size_t)first[o] * 4;
        __m128i acc = round;
        for (int j = 0; j < pairs; j++, k++) {
            __m128i v = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(p + j * 8)), zero);
            acc = _mm_add_epi32(acc, _mm_madd_epi16(_mm_unpacklo_epi16(v, _mm_srli_si128(v, 8)), *k));
        }
        acc = _mm_srai_epi32(acc, 7);
        _mm_storel_epi64((__m128i *)(mid + o * 4), _mm_packs_epi32(acc, acc));
    }
}

/*
 * A destination row of m pixels from rows of the pass across, two pixels
 * at a time, the rows by pairs (with an odd number, the last one twice,
 * weighted 0 the second time).
 */
static void pass_down(const short *const *rows, const __m128i *k, int pairs, int m, unsigned *out)
{
    const __m128i round = _mm_set1_epi32(1 << 20);
    int x = 0;

    for (; x + 2 <= m; x += 2) {
        __m128i lo = round, hi = round;
        for (int j = 0; j < pairs; j++) {
            __m128i a = _mm_loadu_si128((const __m128i *)(rows[2 * j] + x * 4));
            __m128i b = _mm_loadu_si128((const __m128i *)(rows[2 * j + 1] + x * 4));
            lo = _mm_add_epi32(lo, _mm_madd_epi16(_mm_unpacklo_epi16(a, b), k[j]));
            hi = _mm_add_epi32(hi, _mm_madd_epi16(_mm_unpackhi_epi16(a, b), k[j]));
        }
        lo = _mm_packs_epi32(_mm_srai_epi32(lo, 21), _mm_srai_epi32(hi, 21));
        _mm_storel_epi64((__m128i *)(out + x), _mm_packus_epi16(lo, lo));
    }
    if (x < m) {
        __m128i acc = round;
        for (int j = 0; j < pairs; j++) {
            __m128i a = _mm_loadl_epi64((const __m128i *)(rows[2 * j] + x * 4));
            __m128i b = _mm_loadl_epi64((const __m128i *)(rows[2 * j + 1] + x * 4));
            acc = _mm_add_epi32(acc, _mm_madd_epi16(_mm_unpacklo_epi16(a, b), k[j]));
        }
        acc = _mm_packs_epi32(_mm_srai_epi32(acc, 21), acc);
        out[x] = (unsigned)_mm_cvtsi128_si32(_mm_packus_epi16(acc, acc));
    }
}

/* The next 16-aligned offset from n. */
static size_t align16(size_t n)
{
    return (n + 15) & ~(size_t)15;
}

/*
 * Where picture_to_bgra() keeps the shrunk picture (with 8 bytes of slack
 * after it) and resample() its tables and rows, in one block of scratch.
 */
typedef struct {
    size_t first_x, first_y, k_x, k_y, w, tag, rows, mid, size;
} layout_t;

static layout_t layout(int sw, int sh, int w, int h)
{
    int sx = axis_stride(axis_taps(sw, w)), sy = axis_stride(axis_taps(sh, h));
    layout_t l;

    l.first_x = align16((size_t)sw * sh * 4 + 8);
    l.first_y = l.first_x + align16((size_t)w * sizeof(int));
    l.k_x = l.first_y + align16((size_t)h * sizeof(int));
    l.k_y = l.k_x + (size_t)w * (sx / 2) * sizeof(__m128i);
    l.w = l.k_y + (size_t)h * (sy / 2) * sizeof(__m128i);
    l.tag = l.w + align16((size_t)(sx > sy ? sx : sy) * sizeof(short));
    l.rows = l.tag + align16((size_t)sy * sizeof(int));
    l.mid = l.rows + align16((size_t)sy * sizeof(short *));
    l.size = l.mid + (size_t)sy * w * 4 * sizeof(short);
    return l;
}

/*
 * Resamples the sw x sh BGRA picture at the start of mem into w x h at
 * out: across into 16-bit rows, then down. A destination row takes
 * consecutive source rows, later ones for later rows, so the rows across
 * are kept in as many slots as it takes, each made once.
 */
static void resample(int sw, int sh, int w, int h, unsigned *out, unsigned char *mem)
{
    int tx = axis_taps(sw, w), ty = axis_taps(sh, h), px = axis_stride(tx) / 2, py = axis_stride(ty) / 2;
    layout_t l = layout(sw, sh, w, h);
    int *first_x = (int *)(mem + l.first_x), *first_y = (int *)(mem + l.first_y), *tag = (int *)(mem + l.tag);
    __m128i *k_x = (__m128i *)(mem + l.k_x), *k_y = (__m128i *)(mem + l.k_y);
    short *mid = (short *)(mem + l.mid);
    const short **rows = (const short **)(mem + l.rows);

    axis_weights(sw, w, tx, first_x, k_x, (short *)(mem + l.w));
    axis_weights(sh, h, ty, first_y, k_y, (short *)(mem + l.w));
    for (int i = 0; i < 2 * py; i++)
        tag[i] = -1;
    for (int y = 0; y < h; y++) {
        for (int t = 0; t < ty; t++) {
            int r = first_y[y] + t, slot = r % ty;
            short *row = mid + (size_t)slot * w * 4;
            if (tag[slot] != r) {
                pass_across(mem + (size_t)r * sw * 4, w, first_x, k_x, px, row);
                tag[slot] = r;
            }
            rows[t] = row;
        }
        if (ty & 1)
            rows[ty] = rows[ty - 1];
        pass_down(rows, k_y + (size_t)y * py, py, w, out + (size_t)y * w);
    }
}

void picture_to_bgra(const vp8_image_t *img, int w, int h, unsigned *out, picture_scratch_t *scratch)
{
    int s = 1, bw, bh;
    size_t need;

    if (w <= 0 || h <= 0 || img->w <= 0 || img->h <= 0)
        return;
    /* Shrinking by 2 or 4 in YUV, while the result is still no smaller than w x h. */
    while (s < 4 && img->w / (2 * s) >= w && img->h / (2 * s) >= h)
        s *= 2;
    bw = img->w / s;
    bh = img->h / s;
    if (bw == w && bh == h) {
        convert(img, s, out);
        return;
    }
    need = layout(bw, bh, w, h).size;
    if (scratch->size < need) {
        mem_free(scratch->mem);
        scratch->mem = mem_alloc(need);
        scratch->size = need;
    }
    convert(img, s, (unsigned *)scratch->mem);
    resample(bw, bh, w, h, out, scratch->mem);
}

void picture_scratch_free(picture_scratch_t *scratch)
{
    mem_free(scratch->mem);
    scratch->mem = NULL;
    scratch->size = 0;
}
